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
#include "llvm/Analysis/TargetLibraryInfo.h"
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

// 実装を確認して「正常に戻ったときには GC が起きていない」と分かった関数
// (完全一致で照合する)。追加するときは、戻ってくる経路にオブジェクトの
// 解放・移動や ruby_xmalloc 系の確保、Ruby レベルのコードの呼び出しが
// 無いことを確かめる。raise して戻らない経路で GC するのは構わない。
bool isKnownNonGCFunction(StringRef Name) {
  static const StringSet<> Names = {
      // マークと remembered set への記録だけで、確保は素の malloc
      // (デフォルト GC の場合)
      "rb_gc_writebarrier",
      // &errno を返すだけ
      "rb_errno_ptr",
      // libc のシステムコールラッパー
      "fcntl",
      // 以下は GC が起こりうるのが raise して戻らない経路だけなので、
      // 正常に戻ったときは GC していない
      // fptr が NULL なら raise するだけ
      "rb_io_check_initialized",
      // 上記に加え、fd < 0 なら割り込みを処理してから raise する
      "rb_io_check_closed",
      // rb_check_frozen は frozen なら raise する。chilled string の分岐は
      // IO オブジェクトでは通らない
      "rb_io_taint_check",
  };
  return Names.contains(Name);
}

// 第1引数をそのまま返すと実装を確認した関数
bool returnsFirstArgument(const CallBase &CB) {
  static const StringSet<> Names = {
      "rb_io_taint_check",
  };
  const Function *Callee = CB.getCalledFunction();
  return Callee && CB.arg_size() >= 1 && Names.contains(Callee->getName());
}

// libc などの既知のライブラリ関数は Ruby のコードを呼び返さないので GC を
// 起こさない。ただし qsort のように関数ポインタを渡す呼び出しは、
// コールバックが GC を起こしうる。
bool isLibraryCallWithoutCallback(const CallBase &CB, const Function &Callee,
                                  const TargetLibraryInfo &TLI) {
  LibFunc LF;
  if (!TLI.getLibFunc(Callee, LF) || !TLI.has(LF))
    return false;
  return none_of(CB.args(), [](const Use &Arg) {
    return isa<Function>(Arg->stripPointerCasts());
  });
}

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

// v を保持しうるスタック上のスロット (alloca) の候補を集める。
// RB_GC_GUARD(v) で番地を取られた変数は alloca に置かれるので、v は
// そこへ store されるか、そこから load される。phi の場合は、すべての
// incoming 値に共通するスロットだけが v を保持している。
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

// ポインタが指すスロットを返す。RB_GC_GUARD(v) = ... は、スロットの番地を
// 格納した別の alloca から読み出したポインタを通して store する形になる。
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

// スロットへの store (スロットを指すポインタを通したものも含む)
SmallVector<StoreInst *, 8> storesTo(AllocaInst *Slot);

// From から C までの間に、スロットが v 以外の値で上書きされうるか
bool mayBeOverwrittenBetween(AllocaInst *Slot, Instruction *From,
                             const SmallPtrSet<Value *, 8> &Aliases,
                             Instruction *C) {
  for (StoreInst *SI : storesTo(Slot)) {
    if (SI == From)
      continue;
    Value *W = SI->getValueOperand();
    if (Aliases.contains(W) || mergesWith(W, Aliases))
      continue;
    if (isPotentiallyReachable(From, SI) && isPotentiallyReachable(SI, C))
      return true;
  }
  return false;
}

// v の定義から C までの間に、スロットが v 以外の値で上書きされうるか
bool mayBeOverwrittenBefore(AllocaInst *Slot, Value *V,
                            const SmallPtrSet<Value *, 8> &Aliases,
                            Instruction *C) {
  Instruction *Def = dyn_cast<Instruction>(V);
  if (!Def)
    Def = &*C->getFunction()->getEntryBlock().getFirstInsertionPt();
  return mayBeOverwrittenBetween(Slot, Def, Aliases, C);
}

// From から C までの間に、スロットの lifetime が終わりうるか
bool lifetimeMayEndBetween(AllocaInst *Slot, Instruction *From,
                           Instruction *C) {
  for (User *U : Slot->users())
    if (auto *II = dyn_cast<IntrinsicInst>(U);
        II && II->getIntrinsicID() == Intrinsic::lifetime_end &&
        isPotentiallyReachable(From, II) && isPotentiallyReachable(II, C))
      return true;
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

// v が volatile な store でスロットに書かれ、C の時点までスロットに
// 残っているか。volatile な store は省略も移動もされないので、その後
// 上書きも lifetime の終了も無ければ、v は C の時点でスタック上にある
// (file.c の CheckPath のような RB_GC_GUARD(v) = ... の形)。
bool isResidentInSlotAt(Value *A, const SmallPtrSet<Value *, 8> &Aliases,
                        Instruction *C) {
  for (User *U : A->users()) {
    auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || !SI->isVolatile() || SI->getValueOperand() != A)
      continue;
    AllocaInst *Slot = resolveSlot(SI->getPointerOperand());
    if (Slot && isPotentiallyReachable(SI, C) &&
        !mayBeOverwrittenBetween(Slot, SI, Aliases, C) &&
        !lifetimeMayEndBetween(Slot, SI, C))
      return true;
  }
  return false;
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

// C の時点で v が生きているか。次のいずれかなら生きているとみなす。
//   - C 以降に v と等しい値の使用がある
//   - v が volatile な store でスロットに書かれ、C の時点まで残っている
//   - v を保持するスロット (またはそれを指す値) の使用が C 以降にあり、
//     それまでにスロットが上書きされない
bool isLiveAt(Value *V, Instruction *C, const DominatorTree &DT,
              const Instruction &Base) {
  auto Aliases = collectValueAliases(V, &DT, &Base);
  for (Value *A : Aliases) {
    if (hasUseReachableFrom(A, C))
      return true;
    if (isResidentInSlotAt(A, Aliases, C))
      return true;
    for (AllocaInst *Slot : collectSlots(A)) {
      if (mayBeOverwrittenBefore(Slot, A, Aliases, C))
        continue;
      for (Value *H : collectHolders(Slot))
        if (hasUseReachableFrom(H, C))
          return true;
    }
  }
  return false;
}

// v の出どころ。選別の優先度に使う (値が大きいほど優先)。
//   call:     呼び出しの戻り値。このフレームからしか参照されていない可能性が高い
//   argument: 引数。メソッドの receiver など呼び出し元から到達可能なことが多い
//   loaded:   メモリからの読み出し。読み出し元から到達可能なことが多い
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
    // 番地を取られた変数のスロットなら、そこへ store された値をたどる
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
