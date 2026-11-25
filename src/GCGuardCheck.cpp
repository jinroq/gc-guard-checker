// gc-guard-check: RB_GC_GUARD の漏れを検出する LLVM pass
//
// VALUE v から派生したポインタ p (RSTRING_PTR(v) など) を、GC を起こしうる
// 呼び出し C の後で使っているのに、C より後に v の使用が無い (= C の時点で v が
// スタックやレジスタに残っている保証が無い) 箇所を報告する。

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// v を元にしたポインタ (派生ポインタ) を集める
SmallPtrSet<Value *, 16> collectDerivedPointers(IntToPtrInst *Base) {
  SmallPtrSet<Value *, 16> Derived;
  SmallVector<Value *, 16> Worklist{Base};
  while (!Worklist.empty()) {
    Value *P = Worklist.pop_back_val();
    if (!Derived.insert(P).second)
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
collectDerivedAccesses(const SmallPtrSet<Value *, 16> &Derived) {
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

bool mayTriggerGC(const CallBase &CB) {
  if (CB.isInlineAsm() || isa<IntrinsicInst>(CB))
    return false;
  return !CB.onlyReadsMemory();
}

// v を保持するスタック上の場所を集める。RB_GC_GUARD(v) は v を alloca に置き、
// その番地 (を格納した alloca) を asm と volatile load から参照する形になる。
SmallPtrSet<Value *, 8> collectStackHolders(Value *V) {
  SmallPtrSet<Value *, 8> Holders;
  SmallVector<Value *, 8> Worklist;
  for (User *U : V->users())
    if (auto *SI = dyn_cast<StoreInst>(U);
        SI && SI->getValueOperand() == V &&
        isa<AllocaInst>(SI->getPointerOperand()))
      Worklist.push_back(SI->getPointerOperand());
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

// v とビット列が等しい値 (v 自身, inttoptr(v), そのキャストやオフセット 0 の
// GEP) を集める。これらがレジスタやスタックにあれば保守的 GC は v をマークする。
// 一方 RSTRING_PTR の embed 側のような内部ポインタではマークされない。
SmallPtrSet<Value *, 8> collectValueAliases(Value *V) {
  SmallPtrSet<Value *, 8> Aliases;
  SmallVector<Value *, 8> Worklist{V};
  while (!Worklist.empty()) {
    Value *A = Worklist.pop_back_val();
    if (!Aliases.insert(A).second)
      continue;
    for (User *U : A->users()) {
      if (isa<IntToPtrInst, PtrToIntInst, BitCastInst, AddrSpaceCastInst>(U))
        Worklist.push_back(U);
      else if (auto *GEP = dyn_cast<GetElementPtrInst>(U);
               GEP && GEP->hasAllZeroIndices())
        Worklist.push_back(GEP);
    }
  }
  return Aliases;
}

// C の時点で v が生きている (C 以降に v と等しい値またはその保持場所の
// 使用がある) か
bool isLiveAt(Value *V, Instruction *C) {
  for (Value *A : collectValueAliases(V)) {
    if (hasUseReachableFrom(A, C))
      return true;
    for (Value *H : collectStackHolders(A))
      if (hasUseReachableFrom(H, C))
        return true;
  }
  return false;
}

void report(Function &F, Value *V, CallBase &C) {
  raw_ostream &OS = outs();
  OS << "gc-guard-check: " << F.getName() << ": pointer derived from ";
  V->printAsOperand(OS, false);
  OS << " is used after ";
  if (Function *Callee = C.getCalledFunction())
    OS << "call to " << Callee->getName();
  else
    OS << "indirect call";
  OS << " without keeping it alive\n";
}

struct GCGuardCheckPass : PassInfoMixin<GCGuardCheckPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
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
        if (!isPotentiallyReachable(Base, C) || isLiveAt(V, C))
          continue;
        bool UsedAfter = any_of(Accesses, [&](Instruction *U) {
          return isPotentiallyReachable(C, U);
        });
        if (UsedAfter) {
          report(F, V, *C);
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
