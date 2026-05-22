//===- RedzonePass.cpp - instrument memory accesses -----------------------===//
//
// redzone (see ROADMAP.md).
//
// For every `load`/`store` this pass inserts a call to the runtime's
// __redzone_check before the access. It also:
//   * redirects user `malloc`/`free` to __redzone_malloc/__redzone_free so the
//     runtime can guard heap allocations with red zones, and
//   * wraps each static stack allocation with red zones, poisoning them at
//     function entry (__redzone_stack_enter) and unpoisoning before each return
//     (__redzone_stack_leave) so stack-buffer-overflows are caught too.
//
// Source locations (file + line, from debug info) are forwarded into the check
// and malloc calls so the runtime can report the faulting line and the
// allocation site.
//
// It does NOT touch the runtime itself: functions named `__redzone*` are
// skipped (instrumenting them would make the runtime's own malloc recurse).
//
// Run it with opt:
//   opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone -S in.ll
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h" // LLVM 22+
#else
#include "llvm/Passes/PassPlugin.h" // LLVM <= 21
#endif
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>
#include <optional>
#include <utility>

using namespace llvm;

namespace {

static constexpr uint64_t kRedzone = 16; // guard bytes on each side (matches rt)

struct RedzonePass : PassInfoMixin<RedzonePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    LLVMContext &Ctx = M.getContext();
    const DataLayout &DL = M.getDataLayout();

    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *Int8Ty = Type::getInt8Ty(Ctx);
    PointerType *PtrTy = PointerType::getUnqual(Ctx);
    IntegerType *I64 = Type::getInt64Ty(Ctx);
    IntegerType *I32 = Type::getInt32Ty(Ctx);

    FunctionCallee Check = M.getOrInsertFunction(
        "__redzone_check", VoidTy, PtrTy, I64, I32, PtrTy, I32);
    FunctionCallee RzMalloc =
        M.getOrInsertFunction("__redzone_malloc", PtrTy, I64, PtrTy, I32);
    FunctionCallee RzFree =
        M.getOrInsertFunction("__redzone_free", VoidTy, PtrTy);
    FunctionCallee StackEnter =
        M.getOrInsertFunction("__redzone_stack_enter", VoidTy, PtrTy, I64);
    FunctionCallee StackLeave =
        M.getOrInsertFunction("__redzone_stack_leave", VoidTy, PtrTy, I64);
    FunctionCallee RzCalloc = M.getOrInsertFunction(
        "__redzone_calloc", PtrTy, I64, I64, PtrTy, I32);
    FunctionCallee RzRealloc = M.getOrInsertFunction(
        "__redzone_realloc", PtrTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee GlobalRegister =
        M.getOrInsertFunction("__redzone_global_register", VoidTy, PtrTy, I64);

    // Deduplicated filename string globals, keyed by filename.
    StringMap<Constant *> StrCache;
    auto getStr = [&](IRBuilder<> &B, StringRef S) -> Constant * {
      auto It = StrCache.find(S);
      if (It != StrCache.end())
        return It->second;
      Constant *GV = B.CreateGlobalString(S);
      StrCache[S] = GV;
      return GV;
    };
    // (file, line) constants for an instruction's debug location, or (null, 0).
    auto getLoc = [&](IRBuilder<> &B,
                      Instruction *I) -> std::pair<Constant *, Constant *> {
      if (const DebugLoc &Loc = I->getDebugLoc())
        return {getStr(B, Loc->getFilename()), ConstantInt::get(I32, Loc.getLine())};
      return {ConstantPointerNull::get(PtrTy), ConstantInt::get(I32, 0)};
    };

    unsigned checks = 0, mallocs = 0, frees = 0, stackVars = 0, globals = 0;

    // Wrap eligible globals (defined, internal-linkage, non-const) with red
    // zones, and install a constructor that poisons them at startup. Internal
    // linkage keeps this safe: the symbol isn't referenced from other TUs, so
    // changing its type/address can't break anything outside this module.
    {
      SmallVector<GlobalVariable *, 8> targets;
      for (GlobalVariable &G : M.globals()) {
        if (G.isDeclaration() || G.isConstant() || G.isThreadLocal())
          continue;
        if (!G.hasInitializer() || !G.hasLocalLinkage() || G.hasSection())
          continue;
        if (G.getName().starts_with("llvm.") ||
            G.getName().starts_with("__redzone"))
          continue;
        Type *T = G.getValueType();
        if (!T->isSized())
          continue;
        TypeSize TS = DL.getTypeAllocSize(T);
        if (TS.isScalable())
          continue;
        if (G.getAlign() && G.getAlign()->value() > kRedzone)
          continue;
        targets.push_back(&G);
      }

      SmallVector<std::pair<Constant *, uint64_t>, 8> toRegister;
      for (GlobalVariable *G : targets) {
        Type *T = G->getValueType();
        uint64_t S = DL.getTypeAllocSize(T).getFixedValue();
        uint64_t pad = (8 - (S % 8)) % 8; // so data+rightzone is 8-aligned
        ArrayType *LRZTy = ArrayType::get(Int8Ty, kRedzone);
        ArrayType *RRZTy = ArrayType::get(Int8Ty, kRedzone + pad);
        StructType *NTy = StructType::get(Ctx, {LRZTy, T, RRZTy});
        Constant *Init = ConstantStruct::get(
            NTy, {ConstantAggregateZero::get(LRZTy), G->getInitializer(),
                  ConstantAggregateZero::get(RRZTy)});
        auto *NG = new GlobalVariable(M, NTy, /*isConstant=*/false,
                                      G->getLinkage(), Init, G->getName() + ".rz");
        MaybeAlign A = G->getAlign();
        NG->setAlignment(Align(std::max<uint64_t>(kRedzone, A ? A->value() : 1)));

        // Point all users at the data field (struct index 1).
        Constant *Idx[] = {ConstantInt::get(I32, 0), ConstantInt::get(I32, 1)};
        Constant *DataPtr = ConstantExpr::getInBoundsGetElementPtr(NTy, NG, Idx);
        G->replaceAllUsesWith(DataPtr);
        NG->takeName(G);
        G->eraseFromParent();

        toRegister.push_back({DataPtr, S});
        ++globals;
      }

      if (!toRegister.empty()) {
        FunctionType *CtorTy = FunctionType::get(VoidTy, false);
        Function *Ctor = Function::Create(CtorTy, GlobalValue::InternalLinkage,
                                          "__redzone_global_ctor", &M);
        IRBuilder<> CB(BasicBlock::Create(Ctx, "entry", Ctor));
        for (auto &PR : toRegister)
          CB.CreateCall(GlobalRegister,
                        {PR.first, ConstantInt::get(I64, PR.second)});
        CB.CreateRetVoid();
        appendToGlobalCtors(M, Ctor, /*priority=*/0);
      }
    }

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.getName().starts_with("__redzone"))
        continue; // never instrument the runtime

      // Collect first, mutate after, to avoid invalidating iterators.
      SmallVector<Instruction *, 16> accesses;
      SmallVector<CallInst *, 8> mallocCalls, callocCalls, reallocCalls, freeCalls;
      SmallVector<AllocaInst *, 8> allocas;
      SmallVector<ReturnInst *, 4> returns;

      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            accesses.push_back(&I);
          } else if (auto *CI = dyn_cast<CallInst>(&I)) {
            if (Function *Callee = CI->getCalledFunction()) {
              StringRef N = Callee->getName();
              if (N == "malloc")
                mallocCalls.push_back(CI);
              else if (N == "calloc")
                callocCalls.push_back(CI);
              else if (N == "realloc")
                reallocCalls.push_back(CI);
              else if (N == "free")
                freeCalls.push_back(CI);
            }
          } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
            allocas.push_back(AI);
          } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
            returns.push_back(RI);
          }
        }
      }

      // 1. Wrap static stack allocations with red zones.
      for (AllocaInst *AI : allocas) {
        if (!AI->isStaticAlloca())
          continue; // skip variable-length / non-entry allocas
        std::optional<TypeSize> SzOpt = AI->getAllocationSize(DL);
        if (!SzOpt || SzOpt->isScalable())
          continue;
        uint64_t Sz = SzOpt->getFixedValue();
        if (Sz == 0 || AI->getAlign().value() > kRedzone)
          continue; // keep alignment within a red zone's width

        uint64_t total = kRedzone + ((Sz + 7) & ~UINT64_C(7)) + kRedzone;
        IRBuilder<> B(AI);
        AllocaInst *NewAI = B.CreateAlloca(ArrayType::get(Int8Ty, total));
        NewAI->setAlignment(Align(kRedzone));
        Value *UserPtr = B.CreateConstInBoundsGEP1_64(Int8Ty, NewAI, kRedzone);

        AI->replaceAllUsesWith(UserPtr);
        B.CreateCall(StackEnter, {NewAI, ConstantInt::get(I64, Sz)});
        for (ReturnInst *RI : returns) {
          IRBuilder<> RB(RI);
          RB.CreateCall(StackLeave, {NewAI, ConstantInt::get(I64, Sz)});
        }
        AI->eraseFromParent();
        ++stackVars;
      }

      // 2. Insert a check before each load/store.
      for (Instruction *I : accesses) {
        Value *Ptr;
        Type *AccessTy;
        bool IsWrite;
        if (auto *LI = dyn_cast<LoadInst>(I)) {
          Ptr = LI->getPointerOperand();
          AccessTy = LI->getType();
          IsWrite = false;
        } else {
          auto *SI = cast<StoreInst>(I);
          Ptr = SI->getPointerOperand();
          AccessTy = SI->getValueOperand()->getType();
          IsWrite = true;
        }
        uint64_t Size = DL.getTypeStoreSize(AccessTy).getFixedValue();

        IRBuilder<> B(I); // insert the check immediately before the access
        auto [FileC, LineC] = getLoc(B, I);
        B.CreateCall(Check, {Ptr, ConstantInt::get(I64, Size),
                             ConstantInt::get(I32, IsWrite ? 1 : 0), FileC, LineC});
        ++checks;
      }

      // 3. Redirect malloc: rebuild the call with the allocation-site location.
      for (CallInst *CI : mallocCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI =
            B.CreateCall(RzMalloc, {CI->getArgOperand(0), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // calloc(nmemb, size) -> __redzone_calloc(nmemb, size, file, line)
      for (CallInst *CI : callocCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI = B.CreateCall(
            RzCalloc, {CI->getArgOperand(0), CI->getArgOperand(1), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // realloc(ptr, size) -> __redzone_realloc(ptr, size, file, line)
      for (CallInst *CI : reallocCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI = B.CreateCall(
            RzRealloc, {CI->getArgOperand(0), CI->getArgOperand(1), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // 4. Redirect free: same signature, just swap the callee.
      for (CallInst *CI : freeCalls) {
        CI->setCalledFunction(RzFree);
        ++frees;
      }
    }

    errs() << "[redzone] instrumented " << checks << " access(es); " << stackVars
           << " stack var(s); " << globals << " global(s); redirected " << mallocs
           << " alloc / " << frees << " free call(s)\n";

    bool Changed = checks || mallocs || frees || stackVars || globals;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

} // namespace

//===----------------------------------------------------------------------===//
// Plugin registration (New Pass Manager, module pass)
//===----------------------------------------------------------------------===//

llvm::PassPluginLibraryInfo getRedzonePassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "redzone", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // Allow `-passes=redzone` in opt.
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "redzone") {
                    MPM.addPass(RedzonePass());
                    return true;
                  }
                  return false;
                });
            // Allow `clang -fpass-plugin=...` to run us in the default pipeline.
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(RedzonePass());
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getRedzonePassPluginInfo();
}
