// gc-guard-check: an LLVM pass that detects missing RB_GC_GUARD
//
// Reports a pointer p derived from a VALUE v (RSTRING_PTR(v), ...) that is
// used after a call C that may trigger GC, while v is not live at C (so
// nothing guarantees that v is still on the stack or in a register there).

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

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

private:
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
      // Only returns &errno
      "rb_errno_ptr",
      // libc system call wrapper
      "fcntl",
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
// values hold it.
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
      if (In == PN)
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
// and C
bool mayBeOverwrittenBetween(AllocaInst *Slot, Instruction *From,
                             const SmallPtrSet<Value *, 8> &Aliases,
                             Instruction *C, const Reachability &R) {
  for (StoreInst *SI : storesTo(Slot)) {
    if (SI == From)
      continue;
    Value *W = SI->getValueOperand();
    if (Aliases.contains(W) || mergesWith(W, Aliases))
      continue;
    if (R.reaches(From, SI) && R.reaches(SI, C))
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
  return mayBeOverwrittenBetween(Slot, Def, Aliases, C, R);
}

// Whether the lifetime of the slot may end between From and C
bool lifetimeMayEndBetween(AllocaInst *Slot, Instruction *From,
                           Instruction *C, const Reachability &R) {
  for (User *U : Slot->users())
    if (auto *II = dyn_cast<IntrinsicInst>(U);
        II && II->getIntrinsicID() == Intrinsic::lifetime_end &&
        R.reaches(From, II) && R.reaches(II, C))
      return true;
  return false;
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
// stored to memory). RB_GC_GUARD(v) passes it to an asm.
bool isEscaped(AllocaInst *Slot) {
  for (Value *H : collectHolders(Slot))
    for (User *U : H->users()) {
      if (auto *CB = dyn_cast<CallBase>(U);
          CB && CB->hasArgument(H) && !CB->isLifetimeStartOrEnd())
        return true;
      if (auto *SI = dyn_cast<StoreInst>(U);
          SI && SI->getValueOperand() == H &&
          !isa<AllocaInst>(SI->getPointerOperand()))
        return true;
    }
  return false;
}

// Whether v is stored to the slot and stays there until C. For a slot whose
// address has escaped, the compiler must assume that later calls may read it,
// so it cannot remove or delay stores to it (nor volatile stores). Unless the
// slot is overwritten or its lifetime ends afterwards, v is on the stack at C.
// This is why RB_GC_GUARD(v) protects v wherever it is placed, as long as v is
// not reassigned and the slot lives.
bool isResidentInSlotAt(Value *A, const SmallPtrSet<Value *, 8> &Aliases,
                        Instruction *C, const Reachability &R) {
  for (User *U : A->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getValueOperand() != A)
      continue;
    AllocaInst *Slot = resolveSlot(SI->getPointerOperand());
    if (!Slot || !(SI->isVolatile() || isEscaped(Slot)))
      continue;
    if (R.reaches(SI, C) &&
        !mayBeOverwrittenBetween(Slot, SI, Aliases, C, R) &&
        !lifetimeMayEndBetween(Slot, SI, C, R))
      return true;
  }
  return false;
}

// Whether v is loaded from a slot whose address has escaped and stays there
// until C. The slot holds v when it is loaded, and still does at C unless it is
// overwritten or its lifetime ends (e.g. a value reassigned with v = ... after
// FilePathValue(v)).
bool isLoadedFromResidentSlot(Value *A, const SmallPtrSet<Value *, 8> &Aliases,
                              Instruction *C, const Reachability &R) {
  auto *LI = dyn_cast<LoadInst>(A);
  if (!LI)
    return false;
  auto *Slot = dyn_cast<AllocaInst>(LI->getPointerOperand());
  return Slot && isEscaped(Slot) && R.reaches(LI, C) &&
         !mayBeOverwrittenBetween(Slot, LI, Aliases, C, R) &&
         !lifetimeMayEndBetween(Slot, LI, C, R);
}

// Whether v is in a stack slot at C. A phi is when all of its incoming values
// are (whichever path it comes from, that path's value is in the slot).
bool isResidentAt(Value *A, const SmallPtrSet<Value *, 8> &Aliases,
                  Instruction *C, const Reachability &R, unsigned Depth = 0) {
  if (isResidentInSlotAt(A, Aliases, C, R) ||
      isLoadedFromResidentSlot(A, Aliases, C, R))
    return true;
  auto *PN = dyn_cast<PHINode>(A);
  if (!PN || Depth >= 4)
    return false;
  return all_of(PN->incoming_values(), [&](Value *In) {
    return In == PN || isResidentAt(In, Aliases, C, R, Depth + 1);
  });
}

// Whether there is an actual use of X reachable from C (including C itself)
bool hasUseReachableFrom(Value *X, Instruction *C, const Reachability &R) {
  for (User *U : X->users()) {
    auto *I = dyn_cast<Instruction>(U);
    if (!I || I->isLifetimeStartOrEnd())
      continue;
    if (R.reaches(C, I))
      return true;
  }
  return false;
}

// Whether v is live at C. It is considered live if any of the following holds:
//   - a value equal to v is used after C
//   - v is stored to a slot (by a volatile store or a store to a slot whose
//     address has escaped) and stays there until C
//   - v is loaded from a slot whose address has escaped and stays there until C
//   - a use of a slot holding v (or of a value pointing to it) follows C, and
//     the slot is not overwritten before that
bool isLiveAt(Value *V, Instruction *C, const DominatorTree &DT,
              const Instruction &Base, const Reachability &R) {
  auto Aliases = collectValueAliases(V, &DT, &Base);
  for (Value *A : Aliases) {
    if (hasUseReachableFrom(A, C, R))
      return true;
    if (isResidentAt(A, Aliases, C, R))
      return true;
    for (AllocaInst *Slot : collectSlots(A)) {
      if (mayBeOverwrittenBefore(Slot, A, Aliases, C, R))
        continue;
      for (Value *H : collectHolders(Slot))
        if (hasUseReachableFrom(H, C, R))
          return true;
    }
  }
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

struct GCGuardCheckPass : PassInfoMixin<GCGuardCheckPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
    Reachability R(F);
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
      auto Accesses = collectDerivedAccesses(collectDerivedPointers(Base));
      if (!isUsedAsRubyObject(Accesses))
        continue;
      for (CallBase *C : GCCalls) {
        if (!R.reaches(Base, C) || isLiveAt(V, C, DT, *Base, R))
          continue;
        auto *Use = find_if(Accesses, [&](Instruction *U) {
          return R.reaches(C, U);
        });
        if (Use != Accesses.end()) {
          report(F, V, *C, **Use);
          Reported.insert(V);
          break;
        }
      }
    }
    return PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "GCGuardCheck", "0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != "gc-guard-check")
                    return false;
                  FPM.addPass(GCGuardCheckPass());
                  return true;
                });
          }};
}
