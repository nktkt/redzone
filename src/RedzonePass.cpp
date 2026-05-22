//===- RedzonePass.cpp - Phase 1: instrument memory accesses --------------===//
//
// redzone v0.2 (see ROADMAP.md, Horizon 1).
//
// This pass turns redzone into an actual detector. For every `load`/`store` it
// inserts a call to the runtime's __redzone_check before the access, and it
// redirects user `malloc`/`free` calls to __redzone_malloc/__redzone_free so
// the runtime can track allocations and guard them with red zones.
//
// It does NOT touch the runtime itself: functions named `__redzone*` are
// skipped (instrumenting them would make the runtime's own malloc recurse).
//
// Run it with opt:
//   opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone -S in.ll
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h" // moved here in LLVM 22 (was llvm/Passes/)
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

struct RedzonePass : PassInfoMixin<RedzonePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    LLVMContext &Ctx = M.getContext();
    const DataLayout &DL = M.getDataLayout();

    Type *VoidTy = Type::getVoidTy(Ctx);
    PointerType *PtrTy = PointerType::getUnqual(Ctx);
    IntegerType *I64 = Type::getInt64Ty(Ctx);
    IntegerType *I32 = Type::getInt32Ty(Ctx);

    FunctionCallee Check =
        M.getOrInsertFunction("__redzone_check", VoidTy, PtrTy, I64, I32);
    FunctionCallee RzMalloc =
        M.getOrInsertFunction("__redzone_malloc", PtrTy, I64);
    FunctionCallee RzFree =
        M.getOrInsertFunction("__redzone_free", VoidTy, PtrTy);

    unsigned checks = 0, mallocs = 0, frees = 0;

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.getName().starts_with("__redzone"))
        continue; // never instrument the runtime

      // Collect first, mutate after, to avoid invalidating iterators.
      SmallVector<Instruction *, 16> accesses;
      SmallVector<CallInst *, 8> mallocCalls, freeCalls;

      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            accesses.push_back(&I);
          } else if (auto *CI = dyn_cast<CallInst>(&I)) {
            if (Function *Callee = CI->getCalledFunction()) {
              if (Callee->getName() == "malloc")
                mallocCalls.push_back(CI);
              else if (Callee->getName() == "free")
                freeCalls.push_back(CI);
            }
          }
        }
      }

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
        B.CreateCall(Check, {Ptr, ConstantInt::get(I64, Size),
                             ConstantInt::get(I32, IsWrite ? 1 : 0)});
        ++checks;
      }

      for (CallInst *CI : mallocCalls) {
        CI->setCalledFunction(RzMalloc);
        ++mallocs;
      }
      for (CallInst *CI : freeCalls) {
        CI->setCalledFunction(RzFree);
        ++frees;
      }
    }

    errs() << "[redzone] instrumented " << checks << " access(es); redirected "
           << mallocs << " malloc / " << frees << " free call(s)\n";

    bool Changed = checks || mallocs || frees;
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
