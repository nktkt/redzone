//===- RedzonePass.cpp - Phase 0: print every load/store ------------------===//
//
// The first milestone of redzone (see ROADMAP.md, Horizon 1 / v0.1).
//
// This pass does NOT instrument anything yet. It simply walks every function
// and reports each memory `load` and `store` instruction it sees. The point is
// to prove that our pass plugs into the LLVM pipeline and observes exactly the
// memory operations we will later guard with runtime checks.
//
// Build it as a plugin and run it with:
//   opt -load-pass-plugin=build/libRedzonePass.so -passes=redzone \
//       -disable-output input.ll
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h" // moved here in LLVM 22 (was llvm/Passes/)
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

// Print the source location of an instruction if debug info is available
// (compile the input with `-g`). This is groundwork for the readable
// `file:line` reports we want in v0.3.
static void printLocation(const Instruction &I) {
  if (const DebugLoc &DL = I.getDebugLoc()) {
    errs() << " (" << DL->getFilename() << ":" << DL.getLine() << ")";
  }
}

struct RedzonePass : PassInfoMixin<RedzonePass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    // Skip declarations (functions with no body).
    if (F.isDeclaration())
      return PreservedAnalyses::all();

    unsigned loads = 0, stores = 0;
    errs() << "[redzone] function '" << F.getName() << "'\n";

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          ++loads;
          errs() << "    LOAD  from " << *LI->getPointerOperand();
          printLocation(I);
          errs() << "\n";
        } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
          ++stores;
          errs() << "    STORE to  " << *SI->getPointerOperand();
          printLocation(I);
          errs() << "\n";
        }
      }
    }

    errs() << "  -> " << loads << " load(s), " << stores << " store(s)\n";

    // We only observed the IR; nothing was modified.
    return PreservedAnalyses::all();
  }

  // Run even on functions marked `optnone`.
  static bool isRequired() { return true; }
};

} // namespace

//===----------------------------------------------------------------------===//
// Plugin registration (New Pass Manager)
//===----------------------------------------------------------------------===//

llvm::PassPluginLibraryInfo getRedzonePassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "redzone", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "redzone") {
                    FPM.addPass(RedzonePass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getRedzonePassPluginInfo();
}
