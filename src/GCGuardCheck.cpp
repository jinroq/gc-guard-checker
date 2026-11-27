// gc-guard-check: an LLVM pass that detects missing RB_GC_GUARD
//
// Reports a pointer p derived from a VALUE v (RSTRING_PTR(v), ...) that is
// used after a call C that may trigger GC, while v is not live at C (so
// nothing guarantees that v is still on the stack or in a register there).

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>

using namespace llvm;

namespace {

// Reachability between instructions. LLVM's isPotentiallyReachable gives up
// after exploring a limited number of blocks and answers "reachable", which
// causes both false positives and false negatives in large functions. Here
// reachability is unbounded: the set of blocks reachable from each queried
// block is computed on demand and cached.
class Reachability {
public:
  explicit Reachability(Function &F) {
    unsigned N = 0;
    for (BasicBlock &BB : F)
      Index[&BB] = N++;
  }

  // Whether To may be executed after From (true when From == To, rather than
  // asking whether the instruction can be reached again).
  bool reaches(const Instruction *From, const Instruction *To) const {
    if (From == To)
      return true;
    const BasicBlock *FB = From->getParent(), *TB = To->getParent();
    if (FB == TB && From->comesBefore(To))
      return true;
    return successorsReach(FB).test(Index.lookup(TB));
  }

  // Whether To may be executed after From has completed. Unlike reaches, To
  // == From only when the instruction can be executed again (in a loop).
  bool reachesAfter(const Instruction *From, const Instruction *To) const {
    if (From != To)
      return reaches(From, To);
    const BasicBlock *B = From->getParent();
    return successorsReach(B).test(Index.lookup(B));
  }

  // Like reachesAfter, but without executing the definition of X in between.
  // When the definition is executed again in a loop, X is a new value, so
  // what is reached that way is not about the value X had at From.
  bool reachesAfterWithSameValue(const Value *X, const Instruction *From,
                                 const Instruction *To) const {
    if (!reachesAfter(From, To))
      return false;
    auto *Def = dyn_cast<Instruction>(X);
    // A definition outside cycles is executed at most once, so not again if it
    // cannot be executed after From (it may be for the first time)
    if (!Def || (!inCycle(Def->getParent()) && !reachesAfter(From, Def)))
      return true;
    const BasicBlock *FB = From->getParent(), *DB = Def->getParent(),
                     *TB = To->getParent();
    for (auto It = std::next(From->getIterator()); It != FB->end(); ++It) {
      if (&*It == To)
        return true;
      if (&*It == Def)
        return false;
    }
    if (!successorsReachAvoiding(FB, DB).test(Index.lookup(TB)))
      return false;
    // Entering the block of Def, only the instructions before Def are reached
    return TB != DB || To->comesBefore(Def);
  }

private:
  bool inCycle(const BasicBlock *BB) const {
    return successorsReach(BB).test(Index.lookup(BB));
  }

  // The blocks entered from the successors of BB without going through
  // Avoid (which is entered, but not left)
  const BitVector &successorsReachAvoiding(const BasicBlock *BB,
                                           const BasicBlock *Avoid) const {
    auto Key = std::make_pair(BB, Avoid);
    auto It = AvoidingCache.find(Key);
    if (It != AvoidingCache.end())
      return It->second;
    BitVector Seen(Index.size());
    SmallVector<const BasicBlock *, 32> Worklist(succ_begin(BB), succ_end(BB));
    while (!Worklist.empty()) {
      const BasicBlock *B = Worklist.pop_back_val();
      unsigned I = Index.lookup(B);
      if (Seen.test(I))
        continue;
      Seen.set(I);
      if (B != Avoid)
        Worklist.append(succ_begin(B), succ_end(B));
    }
    return AvoidingCache[Key] = std::move(Seen);
  }

  // The blocks reachable from BB through one or more edges
  const BitVector &successorsReach(const BasicBlock *BB) const {
    auto It = Cache.find(BB);
    if (It != Cache.end())
      return It->second;
    BitVector Seen(Index.size());
    SmallVector<const BasicBlock *, 32> Worklist(succ_begin(BB), succ_end(BB));
    while (!Worklist.empty()) {
      const BasicBlock *B = Worklist.pop_back_val();
      unsigned I = Index.lookup(B);
      if (Seen.test(I))
        continue;
      Seen.set(I);
      Worklist.append(succ_begin(B), succ_end(B));
    }
    return Cache[BB] = std::move(Seen);
  }

  DenseMap<const BasicBlock *, unsigned> Index;
  mutable DenseMap<const BasicBlock *, BitVector> Cache;
  mutable DenseMap<std::pair<const BasicBlock *, const BasicBlock *>, BitVector>
      AvoidingCache;
};

// Collects the pointers derived from v. Uses an insertion-ordered set so that
// reports are deterministic.
SmallSetVector<Value *, 16> collectDerivedPointers(IntToPtrInst *Base) {
  SmallSetVector<Value *, 16> Derived;
  SmallVector<Value *, 16> Worklist{Base};
  while (!Worklist.empty()) {
    Value *P = Worklist.pop_back_val();
    if (!Derived.insert(P))
      continue;
    for (User *U : P->users()) {
      if (isa<GetElementPtrInst, PHINode, SelectInst, BitCastInst,
              AddrSpaceCastInst>(U)) {
        Worklist.push_back(U);
      } else if (auto *LI = dyn_cast<LoadInst>(U)) {
        // A pointer to a buffer owned by the object (heap.ptr, ...)
        if (LI->getType()->isPointerTy())
          Worklist.push_back(LI);
      }
    }
  }
  return Derived;
}

// Accesses through the derived pointers. A call receiving a derived pointer as
// an argument also counts, as the callee may read or write through it. If the
// call may trigger GC, the callee may access the pointer after GC (rb_str_new
// allocates the object before copying).
SmallVector<Instruction *, 8>
collectDerivedAccesses(const SmallSetVector<Value *, 16> &Derived) {
  SmallVector<Instruction *, 8> Accesses;
  for (Value *P : Derived)
    for (User *U : P->users()) {
      if (auto *LI = dyn_cast<LoadInst>(U); LI && LI->getPointerOperand() == P)
        Accesses.push_back(LI);
      else if (auto *SI = dyn_cast<StoreInst>(U);
               SI && SI->getPointerOperand() == P)
        Accesses.push_back(SI);
      else if (auto *CB = dyn_cast<CallBase>(U);
               CB && CB->hasArgument(P) && !CB->isLifetimeStartOrEnd())
        Accesses.push_back(CB);
    }
  return Accesses;
}

// Returns the name of the base type of the TBAA access tag (base type, access
// type, offset) if it is a struct
std::optional<StringRef> tbaaBaseTypeName(const Instruction &I) {
  MDNode *Tag = I.getMetadata(LLVMContext::MD_tbaa);
  if (!Tag || Tag->getNumOperands() < 3)
    return std::nullopt;
  auto *Base = dyn_cast<MDNode>(Tag->getOperand(0));
  // A scalar type node is {name, parent}; a struct type node is {name, (type,
  // offset)...}
  if (!Base || Base->getNumOperands() < 3)
    return std::nullopt;
  if (auto *Name = dyn_cast<MDString>(Base->getOperand(0)))
    return Name->getString();
  return std::nullopt;
}

bool isRubyObjectStruct(StringRef Name) {
  static const StringSet<> Names = {
      "RBasic",   "RArray",  "RBignum", "RClass",    "RComplex", "RData",
      "RFile",    "RFloat",  "RHash",   "RMatch",    "RObject",  "RRational",
      "RRegexp",  "RString", "RStruct", "RSymbol",   "RTypedData"};
  return Names.contains(Name);
}

// Whether v is used as a Ruby object. A C struct pointer is sometimes passed as
// a VALUE, so TBAA struct names tell them apart. v is considered not a Ruby
// object only when it is accessed only as non-Ruby structs; without a clue it
// is conservatively treated as a Ruby object.
bool isUsedAsRubyObject(const SmallVector<Instruction *, 8> &Accesses) {
  bool SeenOtherStruct = false;
  for (Instruction *I : Accesses) {
    if (auto Name = tbaaBaseTypeName(*I)) {
      if (isRubyObjectStruct(*Name))
        return true;
      SeenOtherStruct = true;
    }
  }
  return !SeenOtherStruct;
}

// Functions whose implementations were checked not to have triggered GC when
// they return normally (matched exactly). Before adding one, check that the
// paths that return do not free or move objects, allocate through ruby_xmalloc
// and friends, or call Ruby-level code. GC on paths that raise and never return
// is fine.
bool isKnownNonGCFunction(StringRef Name) {
  static const StringSet<> Names = {
      // Only marks and records in the remembered set, allocating with plain
      // malloc (with the default GC)
      "rb_gc_writebarrier",
      // Only greys the object or records it in the remembered set
      "rb_gc_writebarrier_remember",
      // Only returns &errno
      "rb_errno_ptr",
      // Only returns the current execution context
      "rb_current_ec",
      // Only look up an ID table or return the size of an st_table
      // (rb_st_lookup is not here: it may call #hash and #eql? of the keys)
      "rb_id_table_lookup",
      "rb_st_table_size",
      // Only call the C functions of the encoding, or compare bytes
      "rb_enc_precise_mbclen",
      "rb_enc_ascget",
      "rb_memsearch",
      "rb_memcicmp",
      // Only looks up the name table of the regexp
      "onig_name_to_group_numbers",
      // libc system call wrappers that LLVM does not know as library
      // functions (the __*_chk ones are used with _FORTIFY_SOURCE). They
      // never call back into Ruby; a signal handler of Ruby only records
      // the signal.
      "fcntl",
      "send",
      "sendto",
      "recv",
      "__recv_chk",
      "recvfrom",
      "__recvfrom_chk",
      "sleep",
      "pthread_setname_np",
      // zlib's checksums, which only compute over the buffer. Unlike them,
      // deflate and friends allocate through the zalloc of the stream, which
      // the zlib extension sets to a function using xmalloc.
      "crc32",
      "crc32_z",
      "adler32",
      "adler32_z",
      // The following may GC only on paths that raise and never return, so
      // they have not triggered GC when they return normally.
      // Only raises if fptr is NULL
      "rb_io_check_initialized",
      // In addition, processes interrupts and raises if fd < 0
      "rb_io_check_closed",
      // rb_check_frozen raises if frozen. The branch for chilled strings is
      // not taken for IO objects
      "rb_io_taint_check",
  };
  return Names.contains(Name);
}

// Functions checked to return their first argument as is
bool returnsFirstArgument(const CallBase &CB) {
  static const StringSet<> Names = {
      "rb_io_taint_check",
  };
  const Function *Callee = CB.getCalledFunction();
  return Callee && CB.arg_size() >= 1 && Names.contains(Callee->getName());
}

// Library functions known to LLVM, such as libc's, never call back into Ruby,
// so they do not trigger GC. A call passing a function pointer, like qsort, may
// trigger GC through the callback, though.
bool isLibraryCallWithoutCallback(const CallBase &CB, const Function &Callee,
                                  const TargetLibraryInfo &TLI) {
  LibFunc LF;
  if (!TLI.getLibFunc(Callee, LF) || !TLI.has(LF))
    return false;
  return none_of(CB.args(), [](const Use &Arg) {
    return isa<Function>(Arg->stripPointerCasts());
  });
}

// Whether C may trigger GC. GC writes memory, so a call that only reads memory
// (as LLVM's memory attributes tell) does not trigger GC.
bool mayTriggerGC(const CallBase &CB, const TargetLibraryInfo &TLI) {
  if (CB.isInlineAsm() || isa<IntrinsicInst>(CB))
    return false;
  if (CB.onlyReadsMemory())
    return false;
  if (const Function *Callee = CB.getCalledFunction()) {
    if (isKnownNonGCFunction(Callee->getName()))
      return false;
    if (isLibraryCallWithoutCallback(CB, *Callee, TLI))
      return false;
  }
  return true;
}

// If Cmp compares A and W with icmp eq/ne and the "equal" edge of its branch
// dominates Base, returns W
Value *equalOperandDominating(ICmpInst &Cmp, Value *A, const DominatorTree &DT,
                              const Instruction &Base) {
  if (!Cmp.isEquality())
    return nullptr;
  Value *W = Cmp.getOperand(0) == A ? Cmp.getOperand(1) : Cmp.getOperand(0);
  if (isa<Constant>(W))
    return nullptr;
  for (User *U : Cmp.users()) {
    auto *Br = dyn_cast<BranchInst>(U);
    if (!Br || !Br->isConditional())
      continue;
    BasicBlock *EqualSucc =
        Br->getSuccessor(Cmp.getPredicate() == CmpInst::ICMP_EQ ? 0 : 1);
    if (DT.dominates(BasicBlockEdge(Br->getParent(), EqualSucc),
                     Base.getParent()))
      return W;
  }
  return nullptr;
}

// Whether V is known to be a constant (Qnil, Qfalse, ...) at C: C is
// dominated by the edge on which icmp eq V and the constant holds. A constant
// is not an object in the heap, so V need not be kept there (if (!result)
// result = rb_str_new(...) in rb_file_expand_path_internal).
bool isConstantAt(Value *V, const Instruction &C, const DominatorTree &DT) {
  for (User *U : V->users()) {
    auto *Cmp = dyn_cast<ICmpInst>(U);
    if (!Cmp || !Cmp->isEquality())
      continue;
    Value *W = Cmp->getOperand(0) == V ? Cmp->getOperand(1) : Cmp->getOperand(0);
    if (!isa<Constant>(W))
      continue;
    for (User *CU : Cmp->users()) {
      auto *Br = dyn_cast<BranchInst>(CU);
      if (!Br || !Br->isConditional())
        continue;
      BasicBlock *EqualSucc =
          Br->getSuccessor(Cmp->getPredicate() == CmpInst::ICMP_EQ ? 0 : 1);
      if (DT.dominates(BasicBlockEdge(Br->getParent(), EqualSucc),
                       C.getParent()))
        return true;
    }
  }
  return false;
}

// Collects the values that may have the same bits as v (v itself,
// inttoptr(v), its casts, GEPs with zero offset, and phis/selects merging v).
// If any of them is in a register or on the stack, the conservative GC marks
// v. A phi/select is v itself on the paths where v flows in, so if it is
// live, so is v. An interior pointer, such as RSTRING_PTR of an embedded
// string, does not get v marked.
//
// When DT and Base are given, a value W compared with v by icmp eq/ne is also
// an alias if the "equal" edge dominates Base, since the compiler may replace
// v with W there (equality propagation by GVN).
SmallPtrSet<Value *, 8> collectValueAliases(Value *V,
                                            const DominatorTree *DT = nullptr,
                                            const Instruction *Base = nullptr) {
  SmallPtrSet<Value *, 8> Aliases;
  SmallVector<Value *, 8> Worklist{V};
  while (!Worklist.empty()) {
    Value *A = Worklist.pop_back_val();
    if (!Aliases.insert(A).second)
      continue;
    if (auto *CB = dyn_cast<CallBase>(A); CB && returnsFirstArgument(*CB))
      Worklist.push_back(CB->getArgOperand(0));
    for (User *U : A->users()) {
      if (isa<IntToPtrInst, PtrToIntInst, BitCastInst, AddrSpaceCastInst,
              PHINode>(U))
        Worklist.push_back(U);
      else if (auto *SI = dyn_cast<SelectInst>(U);
               SI && SI->getCondition() != A)
        Worklist.push_back(SI);
      else if (auto *GEP = dyn_cast<GetElementPtrInst>(U);
               GEP && GEP->hasAllZeroIndices())
        Worklist.push_back(GEP);
      else if (auto *CB = dyn_cast<CallBase>(U);
               CB && returnsFirstArgument(*CB) && CB->getArgOperand(0) == A)
        Worklist.push_back(CB);
      else if (auto *Cmp = dyn_cast<ICmpInst>(U); Cmp && DT && Base)
        if (Value *W = equalOperandDominating(*Cmp, A, *DT, *Base))
          Worklist.push_back(W);
    }
  }
  return Aliases;
}

// Collects the candidate stack slots (allocas) that may hold v. A variable
// whose address is taken by RB_GC_GUARD(v) lives in an alloca, so v is stored
// to it or loaded from it. For a phi, only the slots common to all incoming
// values hold it. A constant incoming value (Qnil, ...) is not an object in
// the heap, so it imposes no condition (constants also have no use lists).
SmallPtrSet<AllocaInst *, 4> collectSlots(Value *V, unsigned Depth = 0) {
  SmallPtrSet<AllocaInst *, 4> Slots;
  if (auto *LI = dyn_cast<LoadInst>(V); LI && !LI->isVolatile())
    if (auto *AI = dyn_cast<AllocaInst>(LI->getPointerOperand()))
      Slots.insert(AI);
  for (User *U : V->users())
    if (auto *SI = dyn_cast<StoreInst>(U); SI && SI->getValueOperand() == V)
      if (auto *AI = dyn_cast<AllocaInst>(SI->getPointerOperand()))
        Slots.insert(AI);
  if (auto *PN = dyn_cast<PHINode>(V); PN && Depth < 4) {
    std::optional<SmallPtrSet<AllocaInst *, 4>> Common;
    for (Value *In : PN->incoming_values()) {
      if (In == PN || isa<Constant>(In))
        continue;
      auto InSlots = collectSlots(In, Depth + 1);
      if (!Common) {
        Common = InSlots;
        continue;
      }
      SmallVector<AllocaInst *, 4> Gone;
      for (AllocaInst *S : *Common)
        if (!InSlots.contains(S))
          Gone.push_back(S);
      for (AllocaInst *S : Gone)
        Common->erase(S);
    }
    if (Common)
      Slots.insert(Common->begin(), Common->end());
  }
  return Slots;
}

// Whether W is merged with v by a phi/select. If so, on the paths storing W to
// the slot the pointer in use is also made from W (the variable was reassigned,
// as in x = rb_str_new(...)).
bool mergesWith(Value *W, const SmallPtrSet<Value *, 8> &Aliases) {
  if (isa<Constant>(W))
    return false;
  for (Value *A : collectValueAliases(W))
    if (isa<PHINode, SelectInst>(A) && Aliases.contains(A))
      return true;
  return false;
}

// Returns the slot that the pointer points to. RB_GC_GUARD(v) = ... stores
// through a pointer loaded from another alloca that holds the address of the
// slot.
AllocaInst *resolveSlot(Value *Ptr, unsigned Depth = 0) {
  if (auto *AI = dyn_cast<AllocaInst>(Ptr))
    return AI;
  auto *LI = dyn_cast<LoadInst>(Ptr);
  if (!LI || Depth >= 4 || !isa<AllocaInst>(LI->getPointerOperand()))
    return nullptr;
  AllocaInst *Found = nullptr;
  for (User *U : LI->getPointerOperand()->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getPointerOperand() != LI->getPointerOperand())
      continue;
    AllocaInst *Slot = resolveSlot(SI->getValueOperand(), Depth + 1);
    if (!Slot || (Found && Found != Slot))
      return nullptr;
    Found = Slot;
  }
  return Found;
}

// The stores to the slot, including ones through pointers to it
SmallVector<StoreInst *, 8> storesTo(AllocaInst *Slot);

// Whether the slot may be overwritten with a value other than v between From
// and C. A store reached only through the definition of A again (in a loop)
// is followed by a new A, so it does not count.
bool mayBeOverwrittenBetween(AllocaInst *Slot, const Value *A,
                             const Instruction *From,
                             const SmallPtrSet<Value *, 8> &Aliases,
                             Instruction *C, const Reachability &R) {
  for (StoreInst *SI : storesTo(Slot)) {
    if (SI == From)
      continue;
    Value *W = SI->getValueOperand();
    if (Aliases.contains(W) || mergesWith(W, Aliases))
      continue;
    if (R.reachesAfterWithSameValue(A, From, SI) &&
        R.reachesAfterWithSameValue(A, SI, C))
      return true;
  }
  return false;
}

// Whether the slot may be overwritten with a value other than v between the
// definition of v and C
bool mayBeOverwrittenBefore(AllocaInst *Slot, Value *V,
                            const SmallPtrSet<Value *, 8> &Aliases,
                            Instruction *C, const Reachability &R) {
  Instruction *Def = dyn_cast<Instruction>(V);
  if (!Def)
    Def = &*C->getFunction()->getEntryBlock().getFirstInsertionPt();
  return mayBeOverwrittenBetween(Slot, V, Def, Aliases, C, R);
}

// Collects the slot and the values pointing to it. RB_GC_GUARD(v) stores the
// address of the slot to another alloca and refers to it from an asm and a
// volatile load.
SmallPtrSet<Value *, 8> collectHolders(Value *Slot) {
  SmallPtrSet<Value *, 8> Holders;
  SmallVector<Value *, 8> Worklist{Slot};
  while (!Worklist.empty()) {
    Value *H = Worklist.pop_back_val();
    if (!Holders.insert(H).second)
      continue;
    for (User *U : H->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U);
          SI && SI->getValueOperand() == H &&
          isa<AllocaInst>(SI->getPointerOperand()))
        Worklist.push_back(SI->getPointerOperand());
      else if (auto *LI = dyn_cast<LoadInst>(U);
               LI && LI->getType()->isPointerTy())
        Worklist.push_back(LI);
    }
  }
  return Holders;
}

SmallVector<StoreInst *, 8> storesTo(AllocaInst *Slot) {
  SmallVector<StoreInst *, 8> Stores;
  for (Value *H : collectHolders(Slot)) {
    if (resolveSlot(H) != Slot)
      continue;
    for (User *U : H->users())
      if (auto *SI = dyn_cast<StoreInst>(U); SI && SI->getPointerOperand() == H)
        Stores.push_back(SI);
  }
  return Stores;
}

// Whether the address of the slot escapes (is passed to an asm or a call, or
// stored to memory). RB_GC_GUARD(v) passes it to an asm. The address may be
// passed through GEPs, phis and selects (mem = tmpargs in rb_struct_new).
bool isEscaped(AllocaInst *Slot) {
  auto Holders = collectHolders(Slot);
  SmallVector<Value *, 8> Worklist(Holders.begin(), Holders.end());
  SmallPtrSet<Value *, 8> Seen;
  while (!Worklist.empty()) {
    Value *H = Worklist.pop_back_val();
    if (!Seen.insert(H).second)
      continue;
    for (User *U : H->users()) {
      if (auto *CB = dyn_cast<CallBase>(U);
          CB && CB->hasArgument(H) && !CB->isLifetimeStartOrEnd())
        return true;
      if (auto *SI = dyn_cast<StoreInst>(U);
          SI && SI->getValueOperand() == H &&
          !isa<AllocaInst>(SI->getPointerOperand()))
        return true;
      if (isa<GetElementPtrInst, PHINode, SelectInst, BitCastInst,
              AddrSpaceCastInst>(U))
        Worklist.push_back(U);
    }
  }
  return false;
}

// Whether the slot stays in memory, rather than being promoted to registers:
// its address escapes, or it is accessed with volatile loads or stores, as by
// rb_string_value(volatile VALUE *) inlined in string.c. What is stored there
// then stays on the stack until the slot is overwritten.
bool isInMemory(AllocaInst *Slot) {
  if (isEscaped(Slot))
    return true;
  return any_of(Slot->users(), [](User *U) {
    if (auto *LI = dyn_cast<LoadInst>(U))
      return LI->isVolatile();
    if (auto *SI = dyn_cast<StoreInst>(U))
      return SI->isVolatile();
    return false;
  });
}

// The calls that may store to the slot: they receive its address, or a
// pointer to it, as an argument they do not only read (rb_string_value(&v)
// stores the converted value). The asm of RB_GC_GUARD only takes it.
SmallVector<CallBase *, 4> callsWritingTo(AllocaInst *Slot) {
  SmallVector<CallBase *, 4> Calls;
  for (Value *H : collectHolders(Slot))
    for (Use &U : H->uses()) {
      auto *CB = dyn_cast<CallBase>(U.getUser());
      if (!CB || !CB->isArgOperand(&U) || CB->isInlineAsm() ||
          CB->isLifetimeStartOrEnd() || isa<DbgInfoIntrinsic>(CB))
        continue;
      if (CB->onlyReadsMemory(CB->getArgOperandNo(&U)))
        continue;
      Calls.push_back(CB);
    }
  return Calls;
}

// Whether v is in one of the stack slots holding it whenever C is executed
// after v is defined. For a slot whose address has escaped, the compiler must
// assume that later calls may read it, so it cannot remove or delay stores to
// it (nor volatile stores). Unless the slot is overwritten or its lifetime
// ends afterwards, v stored there is on the stack at C. This is why
// RB_GC_GUARD(v) protects v wherever it is placed, as long as v is not
// reassigned and the slot lives. A value loaded from such a slot is in it when
// loaded (e.g. v after FilePathValue(v)).
//
// This follows the paths from the definition of v, tracking the slots that
// hold v, and fails if C is reached with none. On a path where v is not
// stored, the slot holds another value; v may be kept in different slots on
// different paths (link and link_orig in realpath_rec of CRuby). A path that
// executes the definition of v again ends there, since it then makes a new
// value (in the next iteration of a loop).
//
// The paths are followed once for v, recording for each call reached whether
// it may be reached with v in no slot.
enum : uint8_t { CallReached = 1, CallReachedOutOfSlots = 2 };
using SlotScan = DenseMap<const Instruction *, uint8_t>;

SlotScan scanSlots(Value *A, const SmallPtrSet<Value *, 8> &Aliases) {
  SlotScan Result;
  // The state of a path has a bit for each slot, set while the slot holds v,
  // and a bit for each load copying a slot (link_orig = link, which loads link
  // from its slot and stores it), set if the loaded value is v. The content of
  // the slot is taken when it is loaded, as the slot may change before the
  // store. Beyond the limits, slots and copies are not tracked, so v there
  // does not count, but the other slots still do.
  constexpr unsigned MaxSlots = 32, MaxCopies = 32;
  SmallVector<AllocaInst *, 4> Slots;
  auto IndexOf = [&](AllocaInst *Slot) -> std::optional<unsigned> {
    auto It = find(Slots, Slot);
    if (It != Slots.end())
      return unsigned(It - Slots.begin());
    if (Slots.size() >= MaxSlots)
      return std::nullopt;
    Slots.push_back(Slot);
    return unsigned(Slots.size() - 1);
  };
  auto HoldsAcrossCalls = [](StoreInst *SI, AllocaInst *Slot) {
    return Slot && (SI->isVolatile() || isInMemory(Slot));
  };
  constexpr uint64_t SlotBits = (uint64_t(1) << MaxSlots) - 1;
  // What an instruction does to a bit: sets it, clears it, or copies another
  // bit to it
  enum class Kind { Put, Take, Copy };
  struct Effect {
    Kind K;
    unsigned Bit;
    unsigned From = 0;
  };
  DenseMap<const Instruction *, Effect> Effects;
  DenseMap<const Instruction *, uint64_t> CallTakes;
  for (User *U : A->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getValueOperand() != A)
      continue;
    AllocaInst *Slot = resolveSlot(SI->getPointerOperand());
    if (!HoldsAcrossCalls(SI, Slot))
      continue;
    if (auto Index = IndexOf(Slot))
      Effects[SI] = {Kind::Put, *Index};
  }
  uint64_t Initial = 0;
  if (auto *LI = dyn_cast<LoadInst>(A))
    if (auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand());
        Slot && isInMemory(Slot))
      if (auto Index = IndexOf(Slot))
        Initial = uint64_t(1) << *Index;
  // Copies from the slots found so far, which may find more slots
  unsigned Copies = 0;
  for (unsigned I = 0; I < Slots.size(); ++I)
    for (User *U : Slots[I]->users()) {
      auto *LI = dyn_cast<LoadInst>(U);
      // Beyond the limit, copies are not followed: their stores are taken as
      // putting another value (below), as long as the destination is tracked
      if (!LI || LI->getPointerOperand() != Slots[I] || Copies >= MaxCopies)
        continue;
      unsigned LoadBit = MaxSlots + Copies;
      bool Copied = false;
      for (User *LU : LI->users()) {
        auto *SI = dyn_cast<StoreInst>(LU);
        if (!SI || SI->getValueOperand() != LI || Effects.count(SI))
          continue;
        AllocaInst *Slot = resolveSlot(SI->getPointerOperand());
        if (!HoldsAcrossCalls(SI, Slot))
          continue;
        if (auto Index = IndexOf(Slot)) {
          Effects[SI] = {Kind::Copy, *Index, LoadBit};
          Copied = true;
        }
      }
      if (Copied) {
        ++Copies;
        Effects[LI] = {Kind::Copy, LoadBit, I};
      }
    }
  if (Slots.empty())
    return Result;
  for (unsigned I = 0; I < Slots.size(); ++I) {
    for (StoreInst *SI : storesTo(Slots[I])) {
      Value *W = SI->getValueOperand();
      if (Effects.count(SI) || Aliases.contains(W) || mergesWith(W, Aliases))
        continue;
      Effects[SI] = {Kind::Take, I};
    }
    for (User *U : Slots[I]->users())
      if (auto *II = dyn_cast<IntrinsicInst>(U);
          II && II->getIntrinsicID() == Intrinsic::lifetime_end)
        Effects[II] = {Kind::Take, I};
    for (CallBase *CB : callsWritingTo(Slots[I]))
      CallTakes[CB] |= uint64_t(1) << I;
  }

  auto *Def = dyn_cast<Instruction>(A);
  SmallVector<std::pair<const BasicBlock *, uint64_t>, 32> Worklist;
  DenseSet<std::pair<const BasicBlock *, uint64_t>> Seen;
  // Scans BB from It with State
  auto Scan = [&](BasicBlock::const_iterator It, const BasicBlock *BB,
                  uint64_t State) {
    for (; It != BB->end(); ++It) {
      const Instruction *I = &*It;
      if (I == Def)
        return;
      if (isa<CallBase>(I)) {
        Result[I] |= (State & SlotBits) ? CallReached
                                        : CallReached | CallReachedOutOfSlots;
        // A call storing to a slot does so during the call
        if (auto T = CallTakes.find(I); T != CallTakes.end())
          State &= ~T->second;
      }
      if (auto E = Effects.find(I); E != Effects.end()) {
        const Effect &Eff = E->second;
        uint64_t Bit = uint64_t(1) << Eff.Bit;
        bool Set = Eff.K == Kind::Put ||
                   (Eff.K == Kind::Copy && (State & (uint64_t(1) << Eff.From)));
        State = Set ? (State | Bit) : (State & ~Bit);
      }
    }
    for (const BasicBlock *Succ : successors(BB))
      if (Seen.insert({Succ, State}).second)
        Worklist.push_back({Succ, State});
  };
  const Function *F = Def ? Def->getFunction() : cast<Argument>(A)->getParent();
  const BasicBlock *StartBB = Def ? Def->getParent() : &F->getEntryBlock();
  Scan(Def ? std::next(Def->getIterator()) : StartBB->begin(), StartBB,
       Initial);
  while (!Worklist.empty()) {
    auto [BB, State] = Worklist.pop_back_val();
    Scan(BB->begin(), BB, State);
  }
  return Result;
}

// The results of scanSlots, for each value A among the aliases of V
// collected for Base (V and Base decide the aliases: a value may be an alias
// of two arguments of a call, with different aliases)
class SlotScanCache {
public:
  bool isInSlotAt(Value *A, const Value *V,
                  const SmallPtrSet<Value *, 8> &Aliases,
                  const Instruction &Base, const Instruction *C) {
    auto Key = std::make_tuple(static_cast<const Value *>(A), V, &Base);
    auto It = Cache.find(Key);
    if (It == Cache.end())
      It = Cache.try_emplace(Key, scanSlots(A, Aliases)).first;
    uint8_t Flags = It->second.lookup(C);
    return (Flags & CallReached) && !(Flags & CallReachedOutOfSlots);
  }

private:
  DenseMap<std::tuple<const Value *, const Value *, const Instruction *>,
           SlotScan>
      Cache;
};

// Whether v is in a stack slot at C. A phi is when all of its incoming values
// are (whichever path it comes from, that path's value is in the slot), except
// for constants, which are not objects in the heap.
bool isResidentAt(Value *A, const Value *V,
                  const SmallPtrSet<Value *, 8> &Aliases,
                  const Instruction &Base, Instruction *C,
                  SlotScanCache &Slots, unsigned Depth = 0) {
  if (Slots.isInSlotAt(A, V, Aliases, Base, C))
    return true;
  auto *PN = dyn_cast<PHINode>(A);
  if (!PN || Depth >= 4)
    return false;
  return all_of(PN->incoming_values(), [&](Value *In) {
    return In == PN || isa<Constant>(In) ||
           isResidentAt(In, V, Aliases, Base, C, Slots, Depth + 1);
  });
}

// Tells whether a value, observed at an instruction, may come from the value v
// had at C. The values followed are v and those computed from it (the
// pointers derived from it, or the values with the same bits). In a loop, they
// may be computed again for the next iteration, but a phi may carry a value of
// the previous iteration past that (prev = p, prev = cur, or a phi of itself).
class CarriedValues {
public:
  CarriedValues(Value *V, Instruction *C,
                std::function<bool(const Value *)> IsFollowed,
                const Reachability &R)
      : V(V), C(C), IsFollowed(std::move(IsFollowed)), R(R) {}

  // Whether the pointer that the access U goes through (the pointer operand
  // of a load or a store, or an argument of a call) may be derived from v at
  // C. A pointer stored as the value is not accessed through.
  bool isAccessToValueAtC(Instruction *U) {
    if (U == C)
      return true;
    auto Carried = [&](Value *P) { return isFollowed(P) && isCarried(P, U); };
    if (auto *LI = dyn_cast<LoadInst>(U))
      return Carried(LI->getPointerOperand());
    if (auto *SI = dyn_cast<StoreInst>(U))
      return Carried(SI->getPointerOperand());
    if (auto *CB = dyn_cast<CallBase>(U))
      return any_of(CB->args(), [&](Use &Arg) { return Carried(Arg.get()); });
    return false;
  }

  // Whether X at To may be (derived from) v at C
  bool isCarried(Value *X, const Instruction *To) {
    if (!isFollowed(X))
      return false;
    auto *I = dyn_cast<Instruction>(X);
    if (!I)
      return R.reachesAfter(C, To);
    auto Key = std::make_pair(static_cast<const Value *>(X), To);
    if (auto It = Memo.find(Key); It != Memo.end())
      return It->second;
    // Around a cycle of values (q = q + 1 in a loop), assume not carried. A
    // result depending on this assumption for a query further up is not
    // cached, as it may be wrong once that query is answered.
    if (auto It = InProgress.find(Key); It != InProgress.end()) {
      MinAssumedDepth = std::min(MinAssumedDepth, It->second);
      return false;
    }
    unsigned Depth = InProgress.size();
    InProgress[Key] = Depth;
    unsigned OuterMinAssumedDepth = MinAssumedDepth;
    MinAssumedDepth = UINT_MAX;
    bool Result = computeIsCarried(I, To);
    InProgress.erase(Key);
    if (Result || MinAssumedDepth >= Depth)
      Memo[Key] = Result;
    MinAssumedDepth = std::min(OuterMinAssumedDepth, MinAssumedDepth);
    return Result;
  }

private:
  bool isFollowed(const Value *X) const { return X == V || IsFollowed(X); }

  bool computeIsCarried(Instruction *X, const Instruction *To) {
    // Not computed again after C: the value at C (for v itself, the value at C
    // by definition; for others, computed from v)
    if (R.reachesAfterWithSameValue(X, C, To))
      return true;
    // Computed again: from what the operands are at that point
    if (!R.reaches(X, To))
      return false;
    if (auto *PN = dyn_cast<PHINode>(X)) {
      for (unsigned I = 0; I < PN->getNumIncomingValues(); ++I) {
        const Instruction *Term = PN->getIncomingBlock(I)->getTerminator();
        if (R.reachesAfter(C, Term) && isCarried(PN->getIncomingValue(I), Term))
          return true;
      }
      return false;
    }
    // v itself computed again is a new value
    if (X == V)
      return false;
    return R.reachesAfter(C, X) &&
           any_of(X->operands(), [&](Value *Q) { return isCarried(Q, X); });
  }

  Value *V;
  Instruction *C;
  std::function<bool(const Value *)> IsFollowed;
  const Reachability &R;
  DenseMap<std::pair<const Value *, const Instruction *>, bool> Memo;
  // The queries being computed, with their depths, and the least depth of
  // those assumed not carried in the current computation
  DenseMap<std::pair<const Value *, const Instruction *>, unsigned> InProgress;
  unsigned MinAssumedDepth = UINT_MAX;
};

// Whether there is an actual use of H, a slot or a pointer to it, reachable
// from C. Unlike for values with the same bits as v, it does not matter
// whether H is computed again (in a loop), as it is the address of the same
// slot. A use at C itself counts when IncludeC is set, as for
// hasUseOfValueAtC.
bool hasHolderUseAfter(Value *H, Instruction *C, const Reachability &R,
                       bool IncludeC) {
  for (User *U : H->users()) {
    auto *I = dyn_cast<Instruction>(U);
    if (!I || I->isLifetimeStartOrEnd())
      continue;
    if ((IncludeC && I == C) || R.reachesAfter(C, I))
      return true;
  }
  return false;
}

// Whether X, a value with the same bits as v, is used after C while it may
// still be v at C (a use in the next iteration may be of v computed again, or
// of v carried by a phi). A use by a phi happens at the end of the incoming
// block. A use at C itself counts when IncludeC is set: the callee receives v
// and is expected to keep it, as rb_str_resize(str, ...) does.
bool hasUseOfValueAtC(Value *X, Instruction *C, CarriedValues &Carried,
                      bool IncludeC) {
  for (Use &U : X->uses()) {
    auto *I = dyn_cast<Instruction>(U.getUser());
    if (!I || I->isLifetimeStartOrEnd())
      continue;
    if (IncludeC && I == C)
      return true;
    const Instruction *At = I;
    if (auto *PN = dyn_cast<PHINode>(I))
      At = PN->getIncomingBlock(U)->getTerminator();
    if (Carried.isCarried(X, At))
      return true;
  }
  return false;
}

class CallerAnalysis;

// Whether v is live at C. It is considered live if any of the following holds:
//   - a value equal to v is used after C
//   - v is stored to a slot (by a volatile store or a store to a slot whose
//     address has escaped) and stays there until C
//   - v is loaded from a slot whose address has escaped and stays there until C
//   - a use of a slot holding v (or of a value pointing to it) follows C, and
//     the slot is not overwritten before that
//   - a value equal to v is kept alive outside this frame during the whole
//     call of the function (see CallerAnalysis)
bool isLiveAt(Value *V, Instruction *C, const DominatorTree &DT,
              const Instruction &Base, const Reachability &R,
              CallerAnalysis &CA, bool IncludeC = true);

// Tells whether a value in a function is kept alive by its callers during the
// whole call, so that the function does not have to keep it:
//   - The receiver and the arguments of a method defined with
//     rb_define_method and friends are on the VM stack during the call. With
//     argc == -1, so are the values in argv.
//   - An argument of a function with local linkage, all of whose callers are
//     in this module, when every caller keeps the passed value alive across
//     the call (it is live after the call in the caller).
// A function with external linkage, or whose address is used otherwise (as a
// callback, stored in a table, ...), may have unknown callers.
class CallerAnalysis {
public:
  explicit CallerAnalysis(FunctionAnalysisManager &FAM) : FAM(FAM) {}

  bool isKeptAlive(Value *V, unsigned Depth = 0) {
    if (auto *A = dyn_cast<Argument>(V))
      return isArgumentKeptAlive(A);
    if (auto *CB = dyn_cast<CallBase>(V))
      return isReturnedFromKept(*CB, Depth);
    return isLoadedFromMethodArgv(V);
  }

  SlotScanCache &slotScans() { return SlotScans; }

  const Reachability &reachabilityOf(Function &F) {
    auto &Slot = Reach[&F];
    if (!Slot)
      Slot = std::make_unique<Reachability>(F);
    return *Slot;
  }

private:
  struct FunctionUses {
    // Whether all the uses are known: direct calls and method definitions
    bool Known = false;
    SmallVector<CallBase *, 4> Calls;
    // The kinds of argc in the method definitions. With argc >= 0, the
    // receiver and the arguments are on the VM stack; with -1, the receiver
    // (the third argument) and argv; with -2, only the receiver (the first
    // argument), as the arguments are in an Array made for the call.
    bool DefinedWithFixedArgc = false;
    bool DefinedWithArgcM1 = false;
    bool DefinedWithArgcM2 = false;
    bool DefinedWithUnknownArgc = false;

    bool definedAsMethod() const {
      return DefinedWithFixedArgc || DefinedWithArgcM1 || DefinedWithArgcM2 ||
             DefinedWithUnknownArgc;
    }

    // Whether the argument is on the VM stack in every method definition
    bool isArgumentOnVMStack(unsigned ArgNo) const {
      return !DefinedWithUnknownArgc && (!DefinedWithArgcM1 || ArgNo == 2) &&
             (!DefinedWithArgcM2 || ArgNo == 0);
    }

    // Whether argv is on the VM stack in every method definition
    bool isArgvOnVMStack() const {
      return DefinedWithArgcM1 && !DefinedWithFixedArgc && !DefinedWithArgcM2 &&
             !DefinedWithUnknownArgc;
    }
  };

  // For a function defining a method, the indices of the function pointer and
  // argc arguments
  static std::optional<std::pair<unsigned, unsigned>>
  methodDefinitionArgs(StringRef Name) {
    static const StringSet<> WithClass = {
        "rb_define_method",           "rb_define_method_id",
        "rb_define_private_method",   "rb_define_protected_method",
        "rb_define_singleton_method", "rb_define_module_function",
    };
    if (WithClass.contains(Name))
      return std::make_pair(2u, 3u);
    if (Name == "rb_define_global_function")
      return std::make_pair(1u, 2u);
    return std::nullopt;
  }

  const FunctionUses &usesOf(Function *F) {
    auto [It, Inserted] = Uses.try_emplace(F);
    FunctionUses &FU = It->second;
    if (!Inserted)
      return FU;
    if (!F->hasLocalLinkage())
      return FU;
    FU.Known = true;
    for (Use &U : F->uses()) {
      auto *CB = dyn_cast<CallBase>(U.getUser());
      if (CB && CB->isCallee(&U)) {
        FU.Calls.push_back(CB);
        continue;
      }
      const Function *Callee = CB ? CB->getCalledFunction() : nullptr;
      auto Def = Callee ? methodDefinitionArgs(Callee->getName()) : std::nullopt;
      if (!Def || U.getOperandNo() != Def->first ||
          CB->arg_size() <= Def->second) {
        FU.Known = false;
        break;
      }
      auto *Argc = dyn_cast<ConstantInt>(CB->getArgOperand(Def->second));
      int64_t N = Argc ? Argc->getSExtValue() : -3;
      if (N >= 0)
        FU.DefinedWithFixedArgc = true;
      else if (N == -1)
        FU.DefinedWithArgcM1 = true;
      else if (N == -2)
        FU.DefinedWithArgcM2 = true;
      else
        FU.DefinedWithUnknownArgc = true;
    }
    return FU;
  }

  // An argument passed around a cycle of calls is assumed not kept when it is
  // reached again, which only makes more reports. A result that depends on
  // this assumption for an argument further up is not cached, so that the
  // result for each argument does not depend on the order of queries. A true
  // result is cached anyway, since it holds without the assumption.
  bool isArgumentKeptAlive(Argument *A) {
    if (auto It = ArgCache.find(A); It != ArgCache.end())
      return It->second;
    if (auto It = InProgress.find(A); It != InProgress.end()) {
      MinAssumedDepth = std::min(MinAssumedDepth, It->second);
      return false;
    }
    unsigned Depth = InProgress.size();
    InProgress[A] = Depth;
    unsigned OuterMinAssumedDepth = MinAssumedDepth;
    MinAssumedDepth = UINT_MAX;
    const FunctionUses &FU = usesOf(A->getParent());
    bool Kept = FU.Known &&
                (!FU.definedAsMethod() ||
                 FU.isArgumentOnVMStack(A->getArgNo())) &&
                all_of(FU.Calls, [&](CallBase *CB) {
      return CB->arg_size() > A->getArgNo() &&
             isKeptAcrossCall(CB->getArgOperand(A->getArgNo()), CB);
    });
    InProgress.erase(A);
    if (Kept || MinAssumedDepth >= Depth)
      ArgCache[A] = Kept;
    MinAssumedDepth = std::min(OuterMinAssumedDepth, MinAssumedDepth);
    return Kept;
  }

  bool isKeptAcrossCall(Value *Arg, CallBase *CB) {
    // A constant (Qnil, a Fixnum, ...) is not an object made in the heap at
    // run time. (Constants also have no use lists to analyze.)
    if (isa<Constant>(Arg))
      return true;
    Function &Caller = *CB->getFunction();
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(Caller);
    return isLiveAt(Arg, CB, DT, *CB, reachabilityOf(Caller), *this,
                    /*IncludeC=*/false);
  }

  // Whether V is loaded from argv of a method with argc == -1 that is called
  // only by the VM and does not write to argv
  bool isLoadedFromMethodArgv(Value *V) {
    auto *LI = dyn_cast<LoadInst>(V);
    return LI && pointsIntoMethodArgv(LI->getPointerOperand());
  }

  // Whether P points into argv of a method with argc == -1 that is called
  // only by the VM and does not write to argv
  bool pointsIntoMethodArgv(Value *P) {
    while (auto *GEP = dyn_cast<GEPOperator>(P))
      P = GEP->getPointerOperand();
    auto *Argv = dyn_cast<Argument>(P);
    if (!Argv || Argv->getArgNo() != 1 || !Argv->getType()->isPointerTy())
      return false;
    const FunctionUses &FU = usesOf(Argv->getParent());
    return FU.Known && FU.Calls.empty() && FU.isArgvOnVMStack() &&
           !isWrittenThrough(Argv);
  }

  // What a function returns, when it is always the same argument, or a value
  // read through the same argument, which it only reads (as string_options
  // in the prism extension returns argv[0])
  struct ReturnedSource {
    unsigned ArgNo;
    bool ThroughPointer;
  };

  std::optional<ReturnedSource> returnedSource(Function &F) {
    auto [It, Inserted] = Returned.try_emplace(&F);
    if (!Inserted)
      return It->second;
    std::optional<ReturnedSource> Result;
    for (BasicBlock &BB : F) {
      auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
      if (!RI)
        continue;
      Value *RV = RI->getReturnValue();
      std::optional<ReturnedSource> S;
      if (auto *A = dyn_cast_or_null<Argument>(RV)) {
        S = ReturnedSource{A->getArgNo(), false};
      }
      else if (auto *LI = dyn_cast_or_null<LoadInst>(RV)) {
        Value *P = LI->getPointerOperand();
        while (auto *GEP = dyn_cast<GEPOperator>(P))
          P = GEP->getPointerOperand();
        if (auto *A = dyn_cast<Argument>(P); A && A->onlyReadsMemory())
          S = ReturnedSource{A->getArgNo(), true};
      }
      if (!S || (Result && (S->ArgNo != Result->ArgNo ||
                            S->ThroughPointer != Result->ThroughPointer)))
        return Returned[&F] = std::nullopt;
      Result = S;
    }
    return Returned[&F] = Result;
  }

  // Whether the value returned by a call of a function defined here is kept
  // alive: the argument it returns is, or the pointer it reads through
  // points into argv of a method on the VM stack
  bool isReturnedFromKept(CallBase &CB, unsigned Depth) {
    Function *F = CB.getCalledFunction();
    if (!F || F->isDeclaration() || Depth >= 4)
      return false;
    auto Src = returnedSource(*F);
    if (!Src || CB.arg_size() <= Src->ArgNo)
      return false;
    Value *Arg = CB.getArgOperand(Src->ArgNo);
    if (Src->ThroughPointer)
      return pointsIntoMethodArgv(Arg);
    return isa<Constant>(Arg) || isKeptAlive(Arg, Depth + 1);
  }

  // Whether the argument is an array of VALUEs that the function was checked
  // to only read: argv of rb_scan_args and rb_scan_args_kw (unlike the
  // pointers after the format, which they write to)
  static bool onlyReadsValueArray(const CallBase &CB, unsigned ArgNo) {
    static const StringMap<unsigned> ArgvIndex = {
        {"rb_scan_args", 1},
        {"rb_scan_args_kw", 2},
    };
    const Function *Callee = CB.getCalledFunction();
    if (!Callee)
      return false;
    auto It = ArgvIndex.find(Callee->getName());
    return It != ArgvIndex.end() && It->second == ArgNo;
  }

  // Whether something may write through Ptr (or what is derived from it).
  // Any use not known to only read is considered to write: a call may write
  // through the pointer, and so may anything after it is stored to memory.
  static bool isWrittenThrough(Value *Ptr) {
    SmallVector<Value *, 8> Worklist{Ptr};
    SmallPtrSet<Value *, 8> Seen;
    while (!Worklist.empty()) {
      Value *P = Worklist.pop_back_val();
      if (!Seen.insert(P).second)
        continue;
      for (Use &U : P->uses()) {
        User *Usr = U.getUser();
        if (isa<LoadInst, ICmpInst>(Usr))
          continue;
        if (isa<GetElementPtrInst, PHINode, SelectInst>(Usr)) {
          Worklist.push_back(Usr);
          continue;
        }
        if (auto *CB = dyn_cast<CallBase>(Usr)) {
          if (CB->isLifetimeStartOrEnd() || isa<DbgInfoIntrinsic>(CB))
            continue;
          if (CB->isArgOperand(&U)) {
            unsigned ArgNo = CB->getArgOperandNo(&U);
            if ((CB->onlyReadsMemory(ArgNo) && CB->doesNotCapture(ArgNo)) ||
                onlyReadsValueArray(*CB, ArgNo))
              continue;
          }
        }
        return true;
      }
    }
    return false;
  }

  FunctionAnalysisManager &FAM;
  DenseMap<Function *, FunctionUses> Uses;
  DenseMap<Function *, std::optional<ReturnedSource>> Returned;
  DenseMap<Argument *, bool> ArgCache;
  // The arguments being computed, with their depths
  DenseMap<Argument *, unsigned> InProgress;
  // The least depth of the arguments assumed not kept in the computation
  unsigned MinAssumedDepth = UINT_MAX;
  DenseMap<Function *, std::unique_ptr<Reachability>> Reach;
  SlotScanCache SlotScans;
};

bool isLiveAt(Value *V, Instruction *C, const DominatorTree &DT,
              const Instruction &Base, const Reachability &R,
              CallerAnalysis &CA, bool IncludeC) {
  auto Aliases = collectValueAliases(V, &DT, &Base);
  CarriedValues Carried(
      V, C, [&](const Value *X) { return Aliases.contains(X); }, R);
  for (Value *A : Aliases) {
    if (hasUseOfValueAtC(A, C, Carried, IncludeC))
      return true;
    if (isResidentAt(A, V, Aliases, Base, C, CA.slotScans()))
      return true;
    for (AllocaInst *Slot : collectSlots(A)) {
      if (mayBeOverwrittenBefore(Slot, A, Aliases, C, R))
        continue;
      for (Value *H : collectHolders(Slot))
        if (hasHolderUseAfter(H, C, R, IncludeC))
          return true;
    }
  }
  for (Value *A : Aliases)
    if (CA.isKeptAlive(A))
      return true;
  return false;
}

// Where v comes from, used to prioritize triage (greater is higher).
//   call:     returned by a call; likely referenced only from this frame
//   argument: an argument; often reachable from the caller (e.g. a receiver)
//   loaded:   loaded from memory; often reachable from where it was loaded
enum class Origin { Other, Loaded, Argument, Call };

StringRef originName(Origin O) {
  switch (O) {
  case Origin::Call:
    return "call";
  case Origin::Argument:
    return "argument";
  case Origin::Loaded:
    return "loaded";
  case Origin::Other:
    return "other";
  }
  llvm_unreachable("unknown origin");
}

Origin originOf(Value *V, SmallPtrSetImpl<Value *> &Visited) {
  if (!Visited.insert(V).second)
    return Origin::Other;
  if (isa<Argument>(V))
    return Origin::Argument;
  if (auto *CB = dyn_cast<CallBase>(V))
    return returnsFirstArgument(*CB) ? originOf(CB->getArgOperand(0), Visited)
                                     : Origin::Call;
  SmallVector<Value *, 4> Sources;
  if (auto *PN = dyn_cast<PHINode>(V))
    Sources.append(PN->incoming_values().begin(), PN->incoming_values().end());
  else if (auto *SI = dyn_cast<SelectInst>(V))
    Sources.append({SI->getTrueValue(), SI->getFalseValue()});
  else if (auto *LI = dyn_cast<LoadInst>(V)) {
    // For a slot of an address-taken variable, follow the values stored to it
    if (!isa<AllocaInst>(LI->getPointerOperand()))
      return Origin::Loaded;
    for (User *U : LI->getPointerOperand()->users())
      if (auto *St = dyn_cast<StoreInst>(U);
          St && St->getPointerOperand() == LI->getPointerOperand())
        Sources.push_back(St->getValueOperand());
  }
  Origin Best = Origin::Other;
  for (Value *Src : Sources)
    Best = std::max(Best, originOf(Src, Visited));
  return Best;
}

Origin originOf(Value *V) {
  SmallPtrSet<Value *, 8> Visited;
  return originOf(V, Visited);
}

void printLocation(raw_ostream &OS, const Instruction &I) {
  if (const DebugLoc &DL = I.getDebugLoc())
    OS << DL->getFilename() << ":" << DL.getLine();
  else
    OS << "<unknown>";
}

void report(Function &F, Value *V, CallBase &C, Instruction &Use) {
  raw_ostream &OS = outs();
  OS << "gc-guard-check: " << F.getName() << ": pointer derived from ";
  V->printAsOperand(OS, false);
  OS << " is used at ";
  printLocation(OS, Use);
  OS << " after ";
  if (Function *Callee = C.getCalledFunction())
    OS << "call to " << Callee->getName();
  else
    OS << "indirect call";
  OS << " at ";
  printLocation(OS, C);
  OS << " without keeping it alive [origin=" << originName(originOf(V))
     << "]\n";
}

// Confirms a report on one path. The checks above look at the stack slots
// holding v and at the pointers used after C separately, each on some path,
// so they may be satisfied only on different paths: in pack_pack of CRuby, v
// is out of its slot at C only in a later iteration where the call receives
// "" rather than a pointer into v. This follows the paths from the definition
// of v, tracking at once the slots holding v and the values that are v or
// derived from it, and confirms the report if, on one path, C is reached
// with v in no slot and an access through a pointer derived from v follows.
// Each value is computed again on the path from what its operands are then;
// the definition of v executed again makes v a new value, while a pointer
// carried by a phi may still be derived from the previous one (prev = p).
//
// It is used only to drop reports, so it gives up (keeping the report) when
// the state does not fit in its bits or the paths are too many.
class PathConfirmation {
public:
  enum class Result { Confirmed, NotFound, GaveUp };

  PathConfirmation(Value *V, const SmallPtrSet<Value *, 8> &Aliases,
                   const SmallSetVector<Value *, 16> &Derived,
                   const SmallVector<Instruction *, 8> &Accesses)
      : V(V), Def(dyn_cast<Instruction>(V)),
        AccessSet(Accesses.begin(), Accesses.end()) {
    auto Track = [&](Value *X) {
      if (isa<Instruction>(X) || X == V)
        if (!TrackedIndex.count(X) && Tracked.size() < MaxTracked) {
          TrackedIndex[X] = Tracked.size();
          Tracked.push_back(X);
        }
    };
    // Only the values with the same bits as v keep it alive in a slot: a
    // pointer into v there (source_current in rb_str_casemap) does not
    auto TrackSameBits = [&](Value *X) {
      Track(X);
      SameBits.insert(X);
    };
    TrackSameBits(V);
    for (Value *X : Aliases)
      TrackSameBits(X);
    for (Value *X : Derived)
      Track(X);
    // The slots that may hold v: the slot v is loaded from, and those stored
    // a tracked value, including a value loaded from such a slot
    // (link_orig = link)
    auto AddSlot = [&](AllocaInst *Slot) {
      if (SlotIndex.count(Slot))
        return;
      if (Slots.size() >= MaxSlots) {
        Overflow = true;
        return;
      }
      SlotIndex[Slot] = Slots.size();
      Slots.push_back(Slot);
      for (User *SU : Slot->users())
        if (auto *LI = dyn_cast<LoadInst>(SU);
            LI && LI->getPointerOperand() == Slot)
          TrackSameBits(LI);
    };
    if (auto *LI = dyn_cast<LoadInst>(V))
      if (auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand());
          Slot && isInMemory(Slot))
        AddSlot(Slot);
    for (unsigned I = 0; I < Tracked.size() && !Overflow; ++I) {
      for (User *U : Tracked[I]->users()) {
        auto *SI = dyn_cast<StoreInst>(U);
        if (!SI || SI->getValueOperand() != Tracked[I] ||
            !SameBits.count(Tracked[I]))
          continue;
        AllocaInst *Slot = resolveSlot(SI->getPointerOperand());
        if (Slot && (SI->isVolatile() || isInMemory(Slot)))
          AddSlot(Slot);
      }
    }
    if (Overflow)
      return;
    if (Tracked.size() >= MaxTracked)
      Overflow = true;
    if (auto *LI = dyn_cast<LoadInst>(V))
      if (auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()))
        if (auto It = SlotIndex.find(Slot); It != SlotIndex.end())
          InitialSlots = uint64_t(1) << It->second;
    for (AllocaInst *Slot : Slots) {
      for (StoreInst *SI : storesTo(Slot))
        StoreSlot[SI] = SlotIndex[Slot];
      for (CallBase *CB : callsWritingTo(Slot))
        CallTakes[CB] |= uint64_t(1) << SlotIndex[Slot];
    }
  }

  Result confirm(Instruction *C, Instruction *&Use) {
    if (Overflow)
      return Result::GaveUp;
    this->C = C;
    Seen.clear();
    Worklist.clear();
    Steps = 0;
    const BasicBlock *StartBB =
        Def ? Def->getParent()
            : &C->getFunction()->getEntryBlock();
    State Start{InitialSlots, bit(V), false};
    BasicBlock::const_iterator It =
        Def ? std::next(Def->getIterator()) : StartBB->begin();
    if (Def && isa<PHINode>(Def))
      It = StartBB->getFirstNonPHIIt();
    if (scan(StartBB, It, Start, Use))
      return Result::Confirmed;
    while (!Worklist.empty()) {
      if (++Steps > MaxSteps || ++TotalSteps > MaxTotalSteps)
        return Result::GaveUp;
      auto [BB, S] = Worklist.pop_back_val();
      if (scan(BB, BB->getFirstNonPHIIt(), S, Use))
        return Result::Confirmed;
    }
    return Result::NotFound;
  }

private:
  // The limits of the slots and the values tracked, and of the blocks
  // scanned for one call and for all the calls of one v
  static constexpr unsigned MaxSlots = 32, MaxTracked = 64,
                            MaxSteps = 200000, MaxTotalSteps = 1000000;

  struct State {
    uint64_t Slots;
    uint64_t Carried;
    bool Exposed;
  };

  uint64_t bit(const Value *X) const {
    auto It = TrackedIndex.find(X);
    return It == TrackedIndex.end() ? 0 : uint64_t(1) << It->second;
  }
  bool carried(const State &S, const Value *X) const {
    return S.Carried & bit(X);
  }

  // Whether X, computed now from its operands in S, is derived from v
  bool computesFromCarried(const State &S, const Instruction *X) const {
    if (auto *LI = dyn_cast<LoadInst>(X)) {
      if (auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand()))
        if (auto It = SlotIndex.find(Slot); It != SlotIndex.end())
          return S.Slots & (uint64_t(1) << It->second);
      return carried(S, LI->getPointerOperand());
    }
    if (auto *CB = dyn_cast<CallBase>(X))
      return returnsFirstArgument(*CB) && carried(S, CB->getArgOperand(0));
    if (auto *SI = dyn_cast<SelectInst>(X))
      return carried(S, SI->getTrueValue()) ||
             carried(S, SI->getFalseValue());
    return any_of(X->operands(), [&](const Use &Op) {
      return carried(S, Op.get());
    });
  }

  // Whether the access U goes through a pointer derived from v
  bool accessesThroughCarried(const State &S, const Instruction *U) const {
    if (auto *LI = dyn_cast<LoadInst>(U))
      return carried(S, LI->getPointerOperand());
    if (auto *SI = dyn_cast<StoreInst>(U))
      return carried(S, SI->getPointerOperand());
    if (auto *CB = dyn_cast<CallBase>(U))
      return any_of(CB->args(), [&](const Use &A) { return carried(S, A.get()); });
    return false;
  }

  // Scans BB from It in S. Returns true when the report is confirmed.
  bool scan(const BasicBlock *BB, BasicBlock::const_iterator It, State S,
            Instruction *&Use) {
    for (; It != BB->end(); ++It) {
      const Instruction *I = &*It;
      // An exposed path reaching an access, including C itself again (in a
      // loop), through a pointer derived from v
      if (S.Exposed && AccessSet.count(I) && accessesThroughCarried(S, I)) {
        Use = const_cast<Instruction *>(I);
        return true;
      }
      if (I == C && !S.Exposed && !S.Slots) {
        S.Exposed = true;
        if (AccessSet.count(I) && accessesThroughCarried(S, I)) {
          Use = const_cast<Instruction *>(I);
          return true;
        }
      }
      if (auto T = CallTakes.find(I); T != CallTakes.end())
        S.Slots &= ~T->second;
      if (auto *SI = dyn_cast<StoreInst>(I)) {
        if (auto E = StoreSlot.find(SI); E != StoreSlot.end()) {
          uint64_t SlotBit = uint64_t(1) << E->second;
          const Value *W = SI->getValueOperand();
          S.Slots = SameBits.count(W) && carried(S, W) ? (S.Slots | SlotBit)
                                                       : (S.Slots & ~SlotBit);
        }
      }
      else if (auto *II = dyn_cast<IntrinsicInst>(I);
               II && II->getIntrinsicID() == Intrinsic::lifetime_end) {
        if (auto *Slot = dyn_cast<AllocaInst>(II->getArgOperand(
                II->arg_size() - 1)))
          if (auto E = SlotIndex.find(Slot); E != SlotIndex.end())
            S.Slots &= ~(uint64_t(1) << E->second);
      }
      if (uint64_t B = bit(I))
        S.Carried = computesFromCarried(S, I) ? (S.Carried | B)
                                              : (S.Carried & ~B);
    }
    for (const BasicBlock *Succ : successors(BB)) {
      State Next = S;
      // The phis of Succ take the incoming values from BB at once
      uint64_t NewCarried = Next.Carried;
      for (const PHINode &PN : Succ->phis())
        if (uint64_t B = bit(&PN)) {
          bool In = carried(S, PN.getIncomingValueForBlock(BB));
          NewCarried = In ? (NewCarried | B) : (NewCarried & ~B);
        }
      Next.Carried = NewCarried;
      if (Seen.insert({Succ, Next.Slots, Next.Carried, unsigned(Next.Exposed)}).second)
        Worklist.push_back({Succ, Next});
    }
    return false;
  }

  Value *V;
  Instruction *Def;
  Instruction *C = nullptr;
  SmallPtrSet<const Instruction *, 8> AccessSet;
  SmallVector<Value *, 16> Tracked;
  DenseMap<const Value *, unsigned> TrackedIndex;
  SmallPtrSet<const Value *, 16> SameBits;
  SmallVector<AllocaInst *, 4> Slots;
  DenseMap<const AllocaInst *, unsigned> SlotIndex;
  DenseMap<const StoreInst *, unsigned> StoreSlot;
  DenseMap<const Instruction *, uint64_t> CallTakes;
  uint64_t InitialSlots = 0;
  bool Overflow = false;
  unsigned Steps = 0, TotalSteps = 0;
  SmallVector<std::pair<const BasicBlock *, State>, 32> Worklist;
  DenseSet<std::tuple<const BasicBlock *, uint64_t, uint64_t, unsigned>> Seen;
};

// A module pass, since whether an argument is kept alive depends on the
// callers in the module
struct GCGuardCheckPass : PassInfoMixin<GCGuardCheckPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    auto &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    CallerAnalysis CA(FAM);
    for (Function &F : M)
      if (!F.isDeclaration())
        checkFunction(F, FAM, CA);
    return PreservedAnalyses::all();
  }

  static void checkFunction(Function &F, FunctionAnalysisManager &FAM,
                            CallerAnalysis &CA) {
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
    const Reachability &R = CA.reachabilityOf(F);
    SmallVector<CallBase *, 16> GCCalls;
    SmallVector<IntToPtrInst *, 16> Bases;
    for (Instruction &I : instructions(F)) {
      if (auto *CB = dyn_cast<CallBase>(&I); CB && mayTriggerGC(*CB, TLI))
        GCCalls.push_back(CB);
      else if (auto *ITP = dyn_cast<IntToPtrInst>(&I))
        Bases.push_back(ITP);
    }

    SmallPtrSet<Value *, 8> Reported;
    for (IntToPtrInst *Base : Bases) {
      Value *V = Base->getOperand(0);
      if (Reported.contains(V))
        continue;
      auto Derived = collectDerivedPointers(Base);
      auto Accesses = collectDerivedAccesses(Derived);
      if (!isUsedAsRubyObject(Accesses))
        continue;
      std::optional<PathConfirmation> PC;
      for (CallBase *C : GCCalls) {
        if (!R.reaches(Base, C) || isConstantAt(V, *C, DT) ||
            isLiveAt(V, C, DT, *Base, R, CA))
          continue;
        // An access in the next iteration of a loop may be through a new
        // pointer, or one carried from this iteration
        CarriedValues Carried(
            V, C,
            [&](const Value *X) {
              return Derived.contains(const_cast<Value *>(X));
            },
            R);
        auto *Use = find_if(Accesses, [&](Instruction *U) {
          return Carried.isAccessToValueAtC(U);
        });
        if (Use == Accesses.end())
          continue;
        // Drop the report unless it holds on one path
        if (!PC)
          PC.emplace(V, collectValueAliases(V, &DT, Base), Derived, Accesses);
        Instruction *PathUse = nullptr;
        auto Res = PC->confirm(C, PathUse);
        if (Res == PathConfirmation::Result::NotFound)
          continue;
        report(F, V, *C,
               Res == PathConfirmation::Result::Confirmed ? *PathUse : **Use);
        Reported.insert(V);
        break;
      }
    }
  }
  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "GCGuardCheck", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "gc-guard-check")
                    return false;
                  MPM.addPass(GCGuardCheckPass());
                  return true;
                });
          }};
}
