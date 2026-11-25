// gc-guard-check: RB_GC_GUARD の漏れを検出する LLVM pass
//
// VALUE v から派生したポインタ p (RSTRING_PTR(v) など) を、GC を起こしうる
// 呼び出し C の後で使っているのに、C より後に v の使用が無い (= C の時点で v が
// スタックやレジスタに残っている保証が無い) 箇所を報告する。

#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/CFG.h"
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

// v を元にしたポインタ (派生ポインタ) を集める。報告を毎回同じにするため
// 挿入順を保つ集合を使う
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
        // オブジェクトが所有するバッファへのポインタ (heap.ptr など)
        if (LI->getType()->isPointerTy())
          Worklist.push_back(LI);
      }
    }
  }
  return Derived;
}

// 派生ポインタを通したメモリアクセス
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
    }
  return Accesses;
}

// GC はメモリへの書き込みを伴うので、メモリを読むだけの呼び出し (strlen など、
// LLVM が memory 属性を推論できるもの) は GC を起こさない
// TBAA のアクセスタグ (基底型, アクセス型, オフセット) の基底型が構造体なら
// その名前を返す
std::optional<StringRef> tbaaBaseTypeName(const Instruction &I) {
  MDNode *Tag = I.getMetadata(LLVMContext::MD_tbaa);
  if (!Tag || Tag->getNumOperands() < 3)
    return std::nullopt;
  auto *Base = dyn_cast<MDNode>(Tag->getOperand(0));
  // スカラー型のノードは {名前, 親} の2要素、構造体型は {名前, (型, オフセット)...}
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

// v が Ruby オブジェクトとして使われているか。VALUE 型に C の構造体への
// ポインタを詰めることがあるので、TBAA の構造体名で区別する。Ruby 以外の
// 構造体としてのアクセスしか無いときだけ「Ruby オブジェクトではない」とみなし、
// 手がかりが無ければ保守的に Ruby オブジェクトとして扱う。
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

// 実装を確認して GC を起こさないと分かった関数 (完全一致で照合する)。
// 追加するときは、オブジェクトの解放・移動や ruby_xmalloc 系の確保、
// Ruby レベルのコードの呼び出しが無いことを確かめる。
bool isKnownNonGCFunction(StringRef Name) {
  static const StringSet<> Names = {
      // マークと remembered set への記録だけで、確保は素の malloc
      // (デフォルト GC の場合)
      "rb_gc_writebarrier",
      // &errno を返すだけ
      "rb_errno_ptr",
      // libc のシステムコールラッパー
      "fcntl",
  };
  return Names.contains(Name);
}

bool mayTriggerGC(const CallBase &CB) {
  if (CB.isInlineAsm() || isa<IntrinsicInst>(CB))
    return false;
  if (CB.onlyReadsMemory())
    return false;
  if (const Function *Callee = CB.getCalledFunction();
      Callee && isKnownNonGCFunction(Callee->getName()))
    return false;
  return true;
}

// Cmp が A と W を icmp eq/ne で比べ、その分岐の「等しい側」の辺が Base を
// 支配するなら W を返す
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

// v とビット列が等しくなりうる値 (v 自身, inttoptr(v), そのキャストや
// オフセット 0 の GEP, v を合流させる phi/select) を集める。これらが
// レジスタやスタックにあれば保守的 GC は v をマークする。phi/select は
// v が渡ってくる経路では v そのものなので、生きていれば v も生きている。
// 一方 RSTRING_PTR の embed 側のような内部ポインタではマークされない。
//
// DT と Base が与えられたときは、icmp eq/ne で v と比べた値 W も、等しい側の
// 辺が Base を支配するなら別名とみなす。コンパイラはその範囲で v を W に
// 置き換えることがある (GVN の等価性伝播)。
SmallPtrSet<Value *, 8> collectValueAliases(Value *V,
                                            const DominatorTree *DT = nullptr,
                                            const Instruction *Base = nullptr) {
  SmallPtrSet<Value *, 8> Aliases;
  SmallVector<Value *, 8> Worklist{V};
  while (!Worklist.empty()) {
    Value *A = Worklist.pop_back_val();
    if (!Aliases.insert(A).second)
      continue;
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
      else if (auto *Cmp = dyn_cast<ICmpInst>(U); Cmp && DT && Base)
        if (Value *W = equalOperandDominating(*Cmp, A, *DT, *Base))
          Worklist.push_back(W);
    }
  }
  return Aliases;
}

bool isAllocaSlot(Value *P) { return isa<AllocaInst>(P); }

// v を保持しうるスタック上のスロット (alloca) の候補を集める。
// RB_GC_GUARD(v) で番地を取られた変数は alloca に置かれるので、v は
// そこへ store されるか、そこから load される。phi の場合は、すべての
// incoming 値に共通するスロットだけが v を保持している。
SmallPtrSet<Value *, 4> collectSlots(Value *V, unsigned Depth = 0) {
  SmallPtrSet<Value *, 4> Slots;
  if (auto *LI = dyn_cast<LoadInst>(V);
      LI && !LI->isVolatile() && isAllocaSlot(LI->getPointerOperand()))
    Slots.insert(LI->getPointerOperand());
  for (User *U : V->users())
    if (auto *SI = dyn_cast<StoreInst>(U);
        SI && SI->getValueOperand() == V &&
        isAllocaSlot(SI->getPointerOperand()))
      Slots.insert(SI->getPointerOperand());
  if (auto *PN = dyn_cast<PHINode>(V); PN && Depth < 4) {
    std::optional<SmallPtrSet<Value *, 4>> Common;
    for (Value *In : PN->incoming_values()) {
      if (In == PN)
        continue;
      auto InSlots = collectSlots(In, Depth + 1);
      if (!Common) {
        Common = InSlots;
        continue;
      }
      SmallVector<Value *, 4> Gone;
      for (Value *S : *Common)
        if (!InSlots.contains(S))
          Gone.push_back(S);
      for (Value *S : Gone)
        Common->erase(S);
    }
    if (Common)
      Slots.insert(Common->begin(), Common->end());
  }
  return Slots;
}

// W が v と同じ phi/select に合流するか。合流するなら、W をスロットに
// store する経路では、使われるポインタも W から作られている
// (x = rb_str_new(...) のような変数の差し替え)。
bool mergesWith(Value *W, const SmallPtrSet<Value *, 8> &Aliases) {
  if (isa<Constant>(W))
    return false;
  for (Value *A : collectValueAliases(W))
    if (isa<PHINode, SelectInst>(A) && Aliases.contains(A))
      return true;
  return false;
}

// v の定義から C までの間に、スロットが v 以外の値で上書きされうるか
bool mayBeOverwrittenBefore(Value *Slot, Value *V,
                            const SmallPtrSet<Value *, 8> &Aliases,
                            Instruction *C) {
  Instruction *Def = dyn_cast<Instruction>(V);
  if (!Def)
    Def = &*C->getFunction()->getEntryBlock().getFirstInsertionPt();
  for (User *U : Slot->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getPointerOperand() != Slot)
      continue;
    Value *W = SI->getValueOperand();
    if (Aliases.contains(W) || mergesWith(W, Aliases))
      continue;
    if (isPotentiallyReachable(Def, SI) && isPotentiallyReachable(SI, C))
      return true;
  }
  return false;
}

// スロットとそれを指す値を集める。RB_GC_GUARD(v) はスロットの番地を
// 別の alloca に格納し、それを asm と volatile load から参照する形になる。
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
          isAllocaSlot(SI->getPointerOperand()))
        Worklist.push_back(SI->getPointerOperand());
      else if (auto *LI = dyn_cast<LoadInst>(U);
               LI && LI->getType()->isPointerTy())
        Worklist.push_back(LI);
    }
  }
  return Holders;
}

// C から (C 自身も含めて) 到達しうる、X の実際の使用があるか
bool hasUseReachableFrom(Value *X, Instruction *C) {
  for (User *U : X->users()) {
    auto *I = dyn_cast<Instruction>(U);
    if (!I || I->isLifetimeStartOrEnd())
      continue;
    if (I == C || isPotentiallyReachable(C, I))
      return true;
  }
  return false;
}

// C の時点で v が生きている (C 以降に v と等しい値またはその保持場所の
// 使用がある) か
bool isLiveAt(Value *V, Instruction *C, const DominatorTree &DT,
              const Instruction &Base) {
  auto Aliases = collectValueAliases(V, &DT, &Base);
  for (Value *A : Aliases) {
    if (hasUseReachableFrom(A, C))
      return true;
    for (Value *Slot : collectSlots(A)) {
      if (mayBeOverwrittenBefore(Slot, A, Aliases, C))
        continue;
      for (Value *H : collectHolders(Slot))
        if (hasUseReachableFrom(H, C))
          return true;
    }
  }
  return false;
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
  OS << " without keeping it alive\n";
}

struct GCGuardCheckPass : PassInfoMixin<GCGuardCheckPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    SmallVector<CallBase *, 16> GCCalls;
    SmallVector<IntToPtrInst *, 16> Bases;
    for (Instruction &I : instructions(F)) {
      if (auto *CB = dyn_cast<CallBase>(&I); CB && mayTriggerGC(*CB))
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
        if (!isPotentiallyReachable(Base, C) || isLiveAt(V, C, DT, *Base))
          continue;
        auto *Use = find_if(Accesses, [&](Instruction *U) {
          return isPotentiallyReachable(C, U);
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
