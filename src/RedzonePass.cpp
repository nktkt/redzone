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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h" // LLVM 22+
#else
#include "llvm/Passes/PassPlugin.h" // LLVM <= 21
#endif
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>
#include <fnmatch.h>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace {

// Switch the pass into the data-race detector (a distinct, heavier mode; see
// docs/design/data-race-detection.md). Instead of memory-safety checks it emits
// happens-before access hooks and redirects the synchronization primitives.
// Enable with `opt -passes=redzone -redzone-race` or `clang -fpass-plugin=...
// -mllvm -redzone-race`.
static cl::opt<bool>
    ClRaceMode("redzone-race",
               cl::desc("redzone: instrument for data-race detection instead of "
                        "memory-safety checks"),
               cl::init(false));

// Load `fun:<glob>` / `src:<glob>` rules from the file named by the
// REDZONE_IGNORELIST environment variable, if set. Blank lines and `#` comments
// are ignored; unknown prefixes and a missing/unreadable file are skipped
// (best-effort, like a suppressions file). `fun:` matches a function's (mangled)
// name; `src:` matches the translation unit's source file.
static void loadIgnorelist(std::vector<std::string> &funPats,
                           std::vector<std::string> &srcPats) {
  const char *path = getenv("REDZONE_IGNORELIST");
  if (!path)
    return;
  std::ifstream in(path);
  if (!in)
    return;
  std::string line;
  while (std::getline(in, line)) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#')
      continue;
    size_t b = line.find_last_not_of(" \t\r");
    std::string s = line.substr(a, b - a + 1);
    if (s.rfind("fun:", 0) == 0 && s.size() > 4)
      funPats.push_back(s.substr(4));
    else if (s.rfind("src:", 0) == 0 && s.size() > 4)
      srcPats.push_back(s.substr(4));
  }
}

static bool globMatchAny(const std::vector<std::string> &pats, StringRef s) {
  std::string str = s.str();
  for (const std::string &p : pats)
    if (fnmatch(p.c_str(), str.c_str(), 0) == 0)
      return true;
  return false;
}

static constexpr uint64_t kRedzone = 16; // guard bytes on each side (matches rt)

struct RedzonePass : PassInfoMixin<RedzonePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    if (ClRaceMode)
      return runRaceMode(M);

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
    FunctionCallee RzAlignedAlloc = M.getOrInsertFunction(
        "__redzone_aligned_alloc", PtrTy, I64, I64, PtrTy, I32);
    FunctionCallee RzPosixMemalign = M.getOrInsertFunction(
        "__redzone_posix_memalign", I32, PtrTy, I64, I64, PtrTy, I32);
    FunctionCallee RzMemcpy = M.getOrInsertFunction(
        "__redzone_memcpy", PtrTy, PtrTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee RzMemmove = M.getOrInsertFunction(
        "__redzone_memmove", PtrTy, PtrTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee RzMemset = M.getOrInsertFunction(
        "__redzone_memset", PtrTy, PtrTy, I32, I64, PtrTy, I32);
    FunctionCallee RzStrcpy = M.getOrInsertFunction(
        "__redzone_strcpy", PtrTy, PtrTy, PtrTy, PtrTy, I32);
    FunctionCallee RzStrcat = M.getOrInsertFunction(
        "__redzone_strcat", PtrTy, PtrTy, PtrTy, PtrTy, I32);
    FunctionCallee RzStrncpy = M.getOrInsertFunction(
        "__redzone_strncpy", PtrTy, PtrTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee RzStrncat = M.getOrInsertFunction(
        "__redzone_strncat", PtrTy, PtrTy, PtrTy, I64, PtrTy, I32);
    // strlcpy/strlcat return size_t, not the destination pointer.
    FunctionCallee RzStrlcpy = M.getOrInsertFunction(
        "__redzone_strlcpy", I64, PtrTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee RzStrlcat = M.getOrInsertFunction(
        "__redzone_strlcat", I64, PtrTy, PtrTy, I64, PtrTy, I32);
    // Variadic formatted-output wrappers: (dst[, n], file, line, fmt, ...).
    FunctionCallee RzSnprintf = M.getOrInsertFunction(
        "__redzone_snprintf",
        FunctionType::get(I32, {PtrTy, I64, PtrTy, I32, PtrTy}, /*vararg=*/true));
    FunctionCallee RzSprintf = M.getOrInsertFunction(
        "__redzone_sprintf",
        FunctionType::get(I32, {PtrTy, PtrTy, I32, PtrTy}, /*vararg=*/true));
    FunctionCallee GlobalRegister =
        M.getOrInsertFunction("__redzone_global_register", VoidTy, PtrTy, I64);
    FunctionCallee GlobalRegisterRight = M.getOrInsertFunction(
        "__redzone_global_register_right", VoidTy, PtrTy, I64);
    // Base of the direct-mapped shadow, defined by the runtime; loaded inline.
    Constant *ShadowBaseGV = M.getOrInsertGlobal("__redzone_shadow_base", PtrTy);

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

    // The (pointer, byte-size) accessed by a load/store, or nullopt if the size
    // isn't a fixed value (e.g. a scalable vector -- not produced by C/C++).
    auto accessInfo =
        [&](Instruction *I) -> std::optional<std::pair<Value *, uint64_t>> {
      Type *Ty;
      Value *Ptr;
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        Ptr = LI->getPointerOperand();
        Ty = LI->getType();
      } else if (auto *SI = dyn_cast<StoreInst>(I)) {
        Ptr = SI->getPointerOperand();
        Ty = SI->getValueOperand()->getType();
      } else {
        return std::nullopt;
      }
      TypeSize TS = DL.getTypeStoreSize(Ty);
      if (TS.isScalable())
        return std::nullopt;
      return std::make_pair(Ptr, TS.getFixedValue());
    };

    // Selective instrumentation, step 1: is this access provably in-bounds of a
    // *static alloca*? Such an access can never reach a red zone, so checking it
    // is pure overhead -- and dropping the check lets later mem2reg promote the
    // alloca to a register. Restricting this to allocas (not heap or globals)
    // keeps it away from the red zones where bugs actually live, and it MUST be
    // evaluated on the original pointer, before we wrap allocas (afterwards the
    // pointer is offset into a larger, red-zone-padded alloca and an out-of-
    // bounds access would look in-bounds). Mirrors ASan's isSafeAccess.
    auto provablySafeAlloca = [&](Value *Ptr, uint64_t Size) -> bool {
      APInt Off(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
      Value *Base =
          Ptr->stripAndAccumulateConstantOffsets(DL, Off, /*AllowNonInbounds=*/true);
      auto *AI = dyn_cast<AllocaInst>(Base);
      if (!AI || !AI->isStaticAlloca())
        return false;
      std::optional<TypeSize> SzOpt = AI->getAllocationSize(DL);
      if (!SzOpt || SzOpt->isScalable() || Off.isNegative())
        return false;
      uint64_t ObjSize = SzOpt->getFixedValue();
      uint64_t Offset = Off.getZExtValue();
      // [Offset, Offset+Size) must lie within [0, ObjSize).
      return Offset <= ObjSize && ObjSize - Offset >= Size;
    };

    unsigned checks = 0, mallocs = 0, frees = 0, stackVars = 0, globals = 0;
    unsigned skippedSafe = 0, skippedRedundant = 0, optedOutFns = 0, memops = 0;
    unsigned allocPtrs = 0; // address-taken allocators redirected to wrappers

    // Ignore-list: exclude functions/source files matching REDZONE_IGNORELIST
    // rules (like the per-function attribute, but for code you can't annotate).
    // `src:` is matched once against this TU's source file; `fun:` per function.
    std::vector<std::string> ignoreFunPats, ignoreSrcPats;
    loadIgnorelist(ignoreFunPats, ignoreSrcPats);
    bool moduleIgnored = globMatchAny(ignoreSrcPats, M.getSourceFileName());

    // Wrap eligible globals with red zones, and install a constructor that
    // poisons them at startup. Two cases differ in ABI safety:
    //   * INTERNAL linkage: the symbol isn't referenced from other TUs, so we
    //     can change its type/address freely -> red zones on BOTH sides
    //     ({leftrz, data, rightrz}); over- and under-flow are caught.
    //   * EXTERNAL linkage: other TUs reference the symbol, so its address must
    //     not move -> keep data at offset 0 with a TRAILING red zone only
    //     ({data, rightrz}). Cross-TU references still land on the data; only
    //     underflow detection is given up.
    {
      // (global, isExternal) pairs eligible for wrapping.
      SmallVector<std::pair<GlobalVariable *, bool>, 8> targets;
      for (GlobalVariable &G : M.globals()) {
        if (G.isDeclaration() || G.isConstant() || G.isThreadLocal())
          continue;
        if (!G.hasInitializer() || G.hasSection() || G.hasComdat() ||
            G.isExternallyInitialized())
          continue;
        if (G.getName().starts_with("llvm.") ||
            G.getName().starts_with("__redzone"))
          continue;
        // Only two linkages are safe to transform: internal/private (address may
        // move) and plain external (address must be preserved). Skip weak,
        // linkonce, common, appending, etc.
        bool isExternal;
        if (G.hasLocalLinkage())
          isExternal = false;
        else if (G.hasExternalLinkage())
          isExternal = true;
        else
          continue;
        Type *T = G.getValueType();
        if (!T->isSized())
          continue;
        TypeSize TS = DL.getTypeAllocSize(T);
        if (TS.isScalable())
          continue;
        if (G.getAlign() && G.getAlign()->value() > kRedzone)
          continue;
        targets.push_back({&G, isExternal});
      }

      // (dataPtr, size, isExternal) to register at startup.
      SmallVector<std::tuple<Constant *, uint64_t, bool>, 8> toRegister;
      for (auto [G, isExternal] : targets) {
        Type *T = G->getValueType();
        uint64_t S = DL.getTypeAllocSize(T).getFixedValue();
        uint64_t pad = (8 - (S % 8)) % 8; // so the trailing zone stays 8-aligned
        ArrayType *RRZTy = ArrayType::get(Int8Ty, kRedzone + pad);

        StructType *NTy;
        Constant *Init;
        unsigned dataField;
        if (isExternal) {
          // {data, rightrz}: data at offset 0 keeps the symbol address stable.
          NTy = StructType::get(Ctx, {T, RRZTy});
          Init = ConstantStruct::get(
              NTy, {G->getInitializer(), ConstantAggregateZero::get(RRZTy)});
          dataField = 0;
        } else {
          // {leftrz, data, rightrz}: full guarding for an internal symbol.
          ArrayType *LRZTy = ArrayType::get(Int8Ty, kRedzone);
          NTy = StructType::get(Ctx, {LRZTy, T, RRZTy});
          Init = ConstantStruct::get(
              NTy, {ConstantAggregateZero::get(LRZTy), G->getInitializer(),
                    ConstantAggregateZero::get(RRZTy)});
          dataField = 1;
        }

        auto *NG = new GlobalVariable(M, NTy, /*isConstant=*/false,
                                      G->getLinkage(), Init, G->getName() + ".rz");
        MaybeAlign A = G->getAlign();
        NG->setAlignment(Align(std::max<uint64_t>(kRedzone, A ? A->value() : 1)));

        // Point all users at the data field (offset 0 for external).
        Constant *Idx[] = {ConstantInt::get(I32, 0),
                           ConstantInt::get(I32, dataField)};
        Constant *DataPtr = ConstantExpr::getInBoundsGetElementPtr(NTy, NG, Idx);
        G->replaceAllUsesWith(DataPtr);
        NG->takeName(G);
        G->eraseFromParent();

        toRegister.push_back({DataPtr, S, isExternal});
        ++globals;
      }

      if (!toRegister.empty()) {
        FunctionType *CtorTy = FunctionType::get(VoidTy, false);
        Function *Ctor = Function::Create(CtorTy, GlobalValue::InternalLinkage,
                                          "__redzone_global_ctor", &M);
        IRBuilder<> CB(BasicBlock::Create(Ctx, "entry", Ctor));
        for (auto [DataPtr, Size, isExternal] : toRegister)
          CB.CreateCall(isExternal ? GlobalRegisterRight : GlobalRegister,
                        {DataPtr, ConstantInt::get(I64, Size)});
        CB.CreateRetVoid();
        appendToGlobalCtors(M, Ctor, /*priority=*/0);
      }
    }

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.getName().starts_with("__redzone"))
        continue; // never instrument the runtime

      // Exclude this function from access checks and stack red zones if it opts
      // out -- via __attribute__((disable_sanitizer_instrumentation)) (exposed as
      // REDZONE_NO_INSTRUMENT) or a matching REDZONE_IGNORELIST rule -- e.g. a hot
      // path, third-party code, or code that does intentional pointer tricks. Its
      // allocator calls are STILL redirected below, so heap tracking stays
      // consistent (a plain free() of a redzone-allocated pointer would corrupt
      // the heap). We instrument before inlining, so the opt-out holds even if
      // such a function is later inlined into an instrumented one.
      bool skipInstr =
          F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation) ||
          moduleIgnored || globMatchAny(ignoreFunPats, F.getName());
      if (skipInstr)
        ++optedOutFns;

      // Collect first, mutate after, to avoid invalidating iterators.
      SmallVector<Instruction *, 16> accesses;
      SmallVector<CallInst *, 8> mallocCalls, callocCalls, reallocCalls, freeCalls;
      SmallVector<CallInst *, 4> alignedAllocCalls, posixMemalignCalls;
      // C++ new (size arg, like malloc) and delete (ptr arg, like free);
      // alignedNewCalls are C++17 aligned new (size, align) -> aligned_alloc.
      SmallVector<CallInst *, 4> newCalls, alignedNewCalls, deleteCalls;
      // Bulk memory ops: the llvm.mem* intrinsics (the usual form clang emits)
      // and plain memcpy/memmove/memset calls (rare; only when not lowered).
      SmallVector<MemCpyInst *, 4> memCpyOps;
      SmallVector<MemMoveInst *, 4> memMoveOps;
      SmallVector<MemSetInst *, 4> memSetOps;
      SmallVector<CallInst *, 4> memCpyCalls, memMoveCalls, memSetCalls;
      // String copies: 2-arg (strcpy/strcat) and 3-arg (strncpy/strncat), plus
      // their fortified __*_chk forms.
      SmallVector<CallInst *, 4> strCpyCalls, strCatCalls, strNCpyCalls,
          strNCatCalls, strlCpyCalls, strlCatCalls;
      // Formatted output, split by form so the redirect knows the arg layout.
      SmallVector<CallInst *, 4> snprintfCalls, snprintfChkCalls, sprintfCalls,
          sprintfChkCalls;
      SmallVector<AllocaInst *, 8> allocas;
      SmallVector<ReturnInst *, 4> returns;

      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
            accesses.push_back(&I);
          } else if (auto *MI = dyn_cast<MemCpyInst>(&I)) {
            memCpyOps.push_back(MI); // llvm.memcpy (and .inline)
          } else if (auto *MI = dyn_cast<MemMoveInst>(&I)) {
            memMoveOps.push_back(MI);
          } else if (auto *MI = dyn_cast<MemSetInst>(&I)) {
            memSetOps.push_back(MI);
          } else if (auto *CI = dyn_cast<CallInst>(&I)) {
            if (Function *Callee = CI->getCalledFunction()) {
              StringRef N = Callee->getName();
              if (N == "malloc")
                mallocCalls.push_back(CI);
              // Plain calls and the fortified __*_chk forms (macOS/glibc rewrite
              // memcpy/memset to these). The _chk forms carry a 4th object-size
              // arg we ignore -- the shadow knows the real allocation size.
              else if (N == "memcpy" || N == "__memcpy_chk")
                memCpyCalls.push_back(CI);
              else if (N == "memmove" || N == "__memmove_chk")
                memMoveCalls.push_back(CI);
              else if (N == "memset" || N == "__memset_chk")
                memSetCalls.push_back(CI);
              else if (N == "strcpy" || N == "__strcpy_chk")
                strCpyCalls.push_back(CI);
              else if (N == "strcat" || N == "__strcat_chk")
                strCatCalls.push_back(CI);
              else if (N == "strncpy" || N == "__strncpy_chk")
                strNCpyCalls.push_back(CI);
              else if (N == "strncat" || N == "__strncat_chk")
                strNCatCalls.push_back(CI);
              else if (N == "strlcpy" || N == "__strlcpy_chk")
                strlCpyCalls.push_back(CI);
              else if (N == "strlcat" || N == "__strlcat_chk")
                strlCatCalls.push_back(CI);
              else if (N == "snprintf")
                snprintfCalls.push_back(CI);
              else if (N == "__snprintf_chk")
                snprintfChkCalls.push_back(CI);
              else if (N == "sprintf")
                sprintfCalls.push_back(CI);
              else if (N == "__sprintf_chk")
                sprintfChkCalls.push_back(CI);
              else if (N == "calloc")
                callocCalls.push_back(CI);
              else if (N == "realloc")
                reallocCalls.push_back(CI);
              else if (N == "free")
                freeCalls.push_back(CI);
              else if (N == "aligned_alloc")
                alignedAllocCalls.push_back(CI);
              else if (N == "posix_memalign")
                posixMemalignCalls.push_back(CI);
              // Itanium-mangled global operator new / new[] (take a size_t).
              else if (N == "_Znwm" || N == "_Znam")
                newCalls.push_back(CI);
              // C++17 aligned operator new / new[] (size, align_val_t).
              else if (N == "_ZnwmSt11align_val_t" || N == "_ZnamSt11align_val_t")
                alignedNewCalls.push_back(CI);
              // operator delete / delete[]: plain, sized, and C++17 aligned
              // (plain + sized). The first argument is always the pointer.
              else if (N == "_ZdlPv" || N == "_ZdaPv" || N == "_ZdlPvm" ||
                       N == "_ZdaPvm" || N == "_ZdlPvSt11align_val_t" ||
                       N == "_ZdaPvSt11align_val_t" ||
                       N == "_ZdlPvmSt11align_val_t" ||
                       N == "_ZdaPvmSt11align_val_t")
                deleteCalls.push_back(CI);
            }
          } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
            allocas.push_back(AI);
          } else if (auto *RI = dyn_cast<ReturnInst>(&I)) {
            returns.push_back(RI);
          }
        }
      }

      // Redirect allocator / free calls in EVERY function -- even opted-out ones
      // -- so heap tracking stays consistent (a plain free() of a
      // redzone-allocated pointer would corrupt the heap). Steps 3 and 4 below
      // touch only call sites, so they are safe to run before the opt-out check.

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
      // aligned_alloc(align, size) -> __redzone_aligned_alloc(align,size,file,line)
      for (CallInst *CI : alignedAllocCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI = B.CreateCall(
            RzAlignedAlloc,
            {CI->getArgOperand(0), CI->getArgOperand(1), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // posix_memalign(memptr, align, size) ->
      //   __redzone_posix_memalign(memptr, align, size, file, line)
      for (CallInst *CI : posixMemalignCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI = B.CreateCall(
            RzPosixMemalign, {CI->getArgOperand(0), CI->getArgOperand(1),
                              CI->getArgOperand(2), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // C++ operator new / new[] (size arg) -> __redzone_malloc, so new'd blocks
      // get red zones and are tracked just like malloc'd ones.
      for (CallInst *CI : newCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI =
            B.CreateCall(RzMalloc, {CI->getArgOperand(0), FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++mallocs;
      }
      // C++17 aligned operator new / new[] (size, align) ->
      // __redzone_aligned_alloc(align, size, ...). Note the arguments are swapped.
      for (CallInst *CI : alignedNewCalls) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        CallInst *NewCI = B.CreateCall(
            RzAlignedAlloc,
            {CI->getArgOperand(1), CI->getArgOperand(0), FileC, LineC});
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
      // C++ operator delete / delete[] (plain or sized) -> __redzone_free. The
      // sized forms carry an extra size arg, so rebuild the call with just the
      // pointer rather than swapping the callee in place.
      for (CallInst *CI : deleteCalls) {
        IRBuilder<> B(CI);
        B.CreateCall(RzFree, {CI->getArgOperand(0)});
        CI->eraseFromParent();
        ++frees;
      }

      // Excluded functions get no access checks and no stack red zones;
      // everything below is skipped for them.
      if (skipInstr)
        continue;

      // Selective instrumentation: decide which accesses to skip BEFORE any
      // alloca wrapping (which would move pointers into red-zone-padded allocas
      // and defeat the in-bounds analysis). Two sound, false-negative-free rules:
      //   (a) provably in-bounds of a static alloca (can't reach a red zone);
      //   (b) redundant -- the same pointer was already checked, with a >= size,
      //       earlier in this basic block with no intervening call. Nothing but
      //       a call (which may free/realloc/poison) can change the shadow for an
      //       already-validated address, so the later access is still safe.
      SmallPtrSet<Instruction *, 16> skip;
      for (Instruction *I : accesses)
        if (auto AI = accessInfo(I))
          if (provablySafeAlloca(AI->first, AI->second)) {
            skip.insert(I);
            ++skippedSafe;
          }
      for (BasicBlock &BB : F) {
        DenseMap<Value *, uint64_t> checked; // pointer -> max size already checked
        for (Instruction &I : BB) {
          if (isa<CallBase>(I)) {
            checked.clear(); // a call may change the shadow; forget everything
            continue;
          }
          if (skip.count(&I))
            continue; // provably safe: emits no check to rely on
          auto AI = accessInfo(&I);
          if (!AI)
            continue;
          uint64_t &maxChecked = checked[AI->first];
          if (maxChecked >= AI->second && maxChecked != 0) {
            skip.insert(&I);
            ++skippedRedundant;
          } else if (AI->second > maxChecked) {
            maxChecked = AI->second;
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

      // 2. Insert a check before each load/store the analysis didn't skip.
      for (Instruction *I : accesses) {
        if (skip.count(I))
          continue;
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

        IRBuilder<> B(I);
        auto [FileC, LineC] = getLoc(B, I);

        // Inlined fast path. A byte at offset o within its 8-byte chunk is
        // poisoned iff shadow != 0 && o >= shadow (shadow < 0 is full poison;
        // 1..7 is a partial tail). Test the first and (for size > 1) last byte;
        // only on a hit do we call the slow-path __redzone_check to classify and
        // report. The common in-bounds case is a couple of loads and a branch.
        Value *Base = B.CreateLoad(PtrTy, ShadowBaseGV, "rz.base");
        Value *Addr = B.CreatePtrToInt(Ptr, I64);
        auto poisoned = [&](Value *A) -> Value * {
          Value *Idx = B.CreateLShr(A, ConstantInt::get(I64, 3)); // 8 bytes/shadow byte
          Value *Sb = B.CreateLoad(Int8Ty, B.CreateGEP(Int8Ty, Base, Idx));
          Value *Off = B.CreateTrunc(B.CreateAnd(A, ConstantInt::get(I64, 7)), Int8Ty);
          return B.CreateAnd(B.CreateICmpNE(Sb, ConstantInt::get(Int8Ty, 0)),
                             B.CreateICmpSGE(Off, Sb));
        };
        Value *Bad = poisoned(Addr);
        if (Size > 1)
          Bad = B.CreateOr(Bad, poisoned(B.CreateAdd(Addr,
                                                     ConstantInt::get(I64, Size - 1))));

        Instruction *Then =
            SplitBlockAndInsertIfThen(Bad, I->getIterator(), /*Unreachable=*/false);
        IRBuilder<> SB(Then);
        SB.CreateCall(Check, {Ptr, ConstantInt::get(I64, Size),
                              ConstantInt::get(I32, IsWrite ? 1 : 0), FileC, LineC});
        ++checks;
      }

      // Bounds-check bulk memory ops. The intrinsics are void and do the copy
      // themselves, so we REPLACE each with a call to the checking wrapper (which
      // performs the real op); named calls return the destination, so we rebuild
      // and RAUW. The length is normalized to i64.
      auto i64Len = [&](IRBuilder<> &B, Value *Len) -> Value * {
        return B.CreateZExtOrTrunc(Len, I64);
      };
      for (MemCpyInst *MI : memCpyOps) {
        IRBuilder<> B(MI);
        auto [FileC, LineC] = getLoc(B, MI);
        B.CreateCall(RzMemcpy, {MI->getRawDest(), MI->getRawSource(),
                                i64Len(B, MI->getLength()), FileC, LineC});
        MI->eraseFromParent();
        ++memops;
      }
      for (MemMoveInst *MI : memMoveOps) {
        IRBuilder<> B(MI);
        auto [FileC, LineC] = getLoc(B, MI);
        B.CreateCall(RzMemmove, {MI->getRawDest(), MI->getRawSource(),
                                 i64Len(B, MI->getLength()), FileC, LineC});
        MI->eraseFromParent();
        ++memops;
      }
      for (MemSetInst *MI : memSetOps) {
        IRBuilder<> B(MI);
        auto [FileC, LineC] = getLoc(B, MI);
        Value *C = B.CreateZExtOrTrunc(MI->getValue(), I32);
        B.CreateCall(RzMemset, {MI->getRawDest(), C, i64Len(B, MI->getLength()),
                                FileC, LineC});
        MI->eraseFromParent();
        ++memops;
      }
      auto redirectMemCall = [&](CallInst *CI, FunctionCallee Fn, bool isSet) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        Value *Arg1 = isSet ? B.CreateZExtOrTrunc(CI->getArgOperand(1), I32)
                            : CI->getArgOperand(1);
        Value *Len = i64Len(B, CI->getArgOperand(2));
        CallInst *NewCI = B.CreateCall(
            Fn, {CI->getArgOperand(0), Arg1, Len, FileC, LineC});
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++memops;
      };
      for (CallInst *CI : memCpyCalls)
        redirectMemCall(CI, RzMemcpy, /*isSet=*/false);
      for (CallInst *CI : memMoveCalls)
        redirectMemCall(CI, RzMemmove, /*isSet=*/false);
      for (CallInst *CI : memSetCalls)
        redirectMemCall(CI, RzMemset, /*isSet=*/true);

      // String copies. The wrapper derives the length, so we forward only the
      // pointers (and the count for the strn* forms), dropping any __*_chk size.
      auto redirectStr = [&](CallInst *CI, FunctionCallee Fn, bool hasN) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        SmallVector<Value *, 5> args = {CI->getArgOperand(0),
                                        CI->getArgOperand(1)};
        if (hasN)
          args.push_back(i64Len(B, CI->getArgOperand(2)));
        args.push_back(FileC);
        args.push_back(LineC);
        CallInst *NewCI = B.CreateCall(Fn, args);
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++memops;
      };
      for (CallInst *CI : strCpyCalls)
        redirectStr(CI, RzStrcpy, /*hasN=*/false);
      for (CallInst *CI : strCatCalls)
        redirectStr(CI, RzStrcat, /*hasN=*/false);
      for (CallInst *CI : strNCpyCalls)
        redirectStr(CI, RzStrncpy, /*hasN=*/true);
      for (CallInst *CI : strNCatCalls)
        redirectStr(CI, RzStrncat, /*hasN=*/true);
      for (CallInst *CI : strlCpyCalls)
        redirectStr(CI, RzStrlcpy, /*hasN=*/true);
      for (CallInst *CI : strlCatCalls)
        redirectStr(CI, RzStrlcat, /*hasN=*/true);

      // Formatted output: rebuild the variadic call as
      // (dst[, n], file, line, fmt, ...). `hasN` forwards arg 1 as the size
      // (snprintf); `tailStart` is the index of the format string (skipping any
      // __*_chk flag/objsize args, which the wrapper doesn't need).
      auto redirectPrintf = [&](CallInst *CI, FunctionCallee Fn, bool hasN,
                                unsigned tailStart) {
        IRBuilder<> B(CI);
        auto [FileC, LineC] = getLoc(B, CI);
        SmallVector<Value *, 8> args;
        args.push_back(CI->getArgOperand(0)); // dst
        if (hasN)
          args.push_back(i64Len(B, CI->getArgOperand(1))); // n
        args.push_back(FileC);
        args.push_back(LineC);
        for (unsigned i = tailStart; i < CI->arg_size(); i++)
          args.push_back(CI->getArgOperand(i)); // fmt + varargs
        CallInst *NewCI = B.CreateCall(Fn, args);
        NewCI->takeName(CI);
        CI->replaceAllUsesWith(NewCI);
        CI->eraseFromParent();
        ++memops;
      };
      // snprintf(dst, n, fmt, ...) / __snprintf_chk(dst, n, flag, objsize, fmt, ...)
      for (CallInst *CI : snprintfCalls)
        redirectPrintf(CI, RzSnprintf, /*hasN=*/true, /*tailStart=*/2);
      for (CallInst *CI : snprintfChkCalls)
        redirectPrintf(CI, RzSnprintf, /*hasN=*/true, /*tailStart=*/4);
      // sprintf(dst, fmt, ...) / __sprintf_chk(dst, flag, objsize, fmt, ...)
      for (CallInst *CI : sprintfCalls)
        redirectPrintf(CI, RzSprintf, /*hasN=*/false, /*tailStart=*/1);
      for (CallInst *CI : sprintfChkCalls)
        redirectPrintf(CI, RzSprintf, /*hasN=*/false, /*tailStart=*/3);
    }

    // Any allocator use that survives the per-function loop is the allocator's
    // ADDRESS being taken (stored in a function-pointer table and called
    // indirectly -- e.g. cJSON's pluggable hooks `{ malloc, free, realloc }`),
    // which the call-site redirection above can't see. Replace those remaining
    // uses with malloc-compatible wrappers so indirectly-allocated blocks are
    // still red-zoned. Direct calls were already rewritten, so only value uses
    // are left; the wrapper takes the allocator's own type so the replacement is
    // type-correct.
    {
      struct PtrRedir {
        const char *name, *wrapper;
      };
      static const PtrRedir kAllocPtr[] = {
          {"malloc", "__redzone_malloc_plain"},
          {"calloc", "__redzone_calloc_plain"},
          {"realloc", "__redzone_realloc_plain"},
          {"free", "__redzone_free"}, // already matches free's signature
      };
      for (const PtrRedir &R : kAllocPtr) {
        Function *AF = M.getFunction(R.name);
        if (!AF || AF->use_empty())
          continue;
        FunctionCallee W = M.getOrInsertFunction(R.wrapper, AF->getFunctionType());
        AF->replaceAllUsesWith(W.getCallee());
        ++allocPtrs;
      }
    }

    errs() << "[redzone] instrumented " << checks << " access(es) (skipped "
           << skippedSafe << " safe + " << skippedRedundant << " redundant); "
           << stackVars << " stack var(s); " << globals << " global(s); redirected "
           << mallocs << " alloc / " << frees << " free call(s) / " << memops
           << " mem op(s) / " << allocPtrs << " alloc-ptr(s); " << optedOutFns
           << " opted-out fn(s)\n";

    bool Changed = checks || mallocs || frees || stackVars || globals || memops ||
                   allocPtrs;
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }

  //===--------------------------------------------------------------------===//
  // Data-race detection mode (see docs/design/data-race-detection.md).
  //
  // A separate, heavier instrumentation: before every load/store emit a call to
  // the race runtime (rz_rt_read / rz_rt_write), and redirect the pthread
  // synchronization primitives to wrappers that record happens-before edges. It
  // shares nothing with the address checker -- no shadow check, no red zones, no
  // allocator redirection -- and links a different runtime
  // (runtime/redzone_race*.c). This runs best on already-optimized IR so that
  // promotable locals are in registers and only genuine memory accesses remain;
  // instrumenting a thread-local stack slot is merely overhead (it can't race),
  // never a correctness problem.
  //===--------------------------------------------------------------------===//
  PreservedAnalyses runRaceMode(Module &M) {
    LLVMContext &Ctx = M.getContext();
    const DataLayout &DL = M.getDataLayout();
    Type *VoidTy = Type::getVoidTy(Ctx);
    PointerType *PtrTy = PointerType::getUnqual(Ctx);
    IntegerType *I64 = Type::getInt64Ty(Ctx);
    IntegerType *I32 = Type::getInt32Ty(Ctx);

    FunctionCallee RaceRead =
        M.getOrInsertFunction("rz_rt_read", VoidTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee RaceWrite =
        M.getOrInsertFunction("rz_rt_write", VoidTy, PtrTy, I64, PtrTy, I32);
    FunctionCallee AtomicAcquire =
        M.getOrInsertFunction("rz_rt_atomic_acquire", VoidTy, PtrTy);
    FunctionCallee AtomicRelease =
        M.getOrInsertFunction("rz_rt_atomic_release", VoidTy, PtrTy);

    // Deduplicated filename globals + (file, line) for an instruction's debug
    // location, mirroring address mode so race reports carry source positions.
    StringMap<Constant *> StrCache;
    auto getStr = [&](IRBuilder<> &B, StringRef S) -> Constant * {
      auto It = StrCache.find(S);
      if (It != StrCache.end())
        return It->second;
      Constant *GV = B.CreateGlobalString(S);
      StrCache[S] = GV;
      return GV;
    };
    auto getLoc = [&](IRBuilder<> &B,
                      Instruction *I) -> std::pair<Constant *, Constant *> {
      if (const DebugLoc &Loc = I->getDebugLoc())
        return {getStr(B, Loc->getFilename()),
                ConstantInt::get(I32, Loc.getLine())};
      return {ConstantPointerNull::get(PtrTy), ConstantInt::get(I32, 0)};
    };

    // pthread primitive -> race-runtime wrapper. The wrappers still perform the
    // real operation; they just also record the ordering edge.
    struct Redir {
      const char *from, *to;
    };
    static const Redir kSync[] = {
        {"pthread_create", "rz_rt_pthread_create"},
        {"pthread_join", "rz_rt_pthread_join"},
        {"pthread_mutex_lock", "rz_rt_pthread_mutex_lock"},
        {"pthread_mutex_unlock", "rz_rt_pthread_mutex_unlock"},
        {"pthread_mutex_trylock", "rz_rt_pthread_mutex_trylock"},
        {"pthread_rwlock_rdlock", "rz_rt_pthread_rwlock_rdlock"},
        {"pthread_rwlock_wrlock", "rz_rt_pthread_rwlock_wrlock"},
        {"pthread_rwlock_tryrdlock", "rz_rt_pthread_rwlock_tryrdlock"},
        {"pthread_rwlock_trywrlock", "rz_rt_pthread_rwlock_trywrlock"},
        {"pthread_rwlock_unlock", "rz_rt_pthread_rwlock_unlock"},
        {"pthread_cond_wait", "rz_rt_pthread_cond_wait"},
        {"pthread_cond_timedwait", "rz_rt_pthread_cond_timedwait"},
    };
    auto syncTarget = [&](StringRef N) -> const char * {
      // macOS headers give some pthread entry points an assembler label such as
      // "\01_pthread_join"; normalize that to the plain name before matching, or
      // the redirect silently misses (a missing join/lock edge -> false report).
      if (N.consume_front("\x01"))
        N.consume_front("_");
      for (const Redir &R : kSync)
        if (N == R.from)
          return R.to;
      return nullptr;
    };

    unsigned accesses = 0, syncs = 0, atomics = 0;

    // Emit a plain (non-atomic) data-access hook before instruction `At`.
    auto emitAccess = [&](Value *Ptr, Type *Ty, bool isWrite, Instruction *At) {
      TypeSize TS = DL.getTypeStoreSize(Ty);
      if (TS.isScalable())
        return; // not produced by C/C++
      IRBuilder<> B(At);
      auto [FileC, LineC] = getLoc(B, At);
      B.CreateCall(isWrite ? RaceWrite : RaceRead,
                   {Ptr, ConstantInt::get(I64, TS.getFixedValue()), FileC, LineC});
      ++accesses;
    };

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      StringRef FN = F.getName();
      // Never instrument the detector's own runtime (rz_*) or the address
      // runtime (__redzone*) should they ever share a module.
      if (FN.starts_with("__redzone") || FN.starts_with("rz_"))
        continue;

      SmallVector<Instruction *, 32> mem;
      SmallVector<CallInst *, 8> syncCalls;
      for (BasicBlock &BB : F)
        for (Instruction &I : BB) {
          if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<AtomicRMWInst>(I) ||
              isa<AtomicCmpXchgInst>(I))
            mem.push_back(&I);
          else if (auto *CI = dyn_cast<CallInst>(&I))
            if (Function *Cee = CI->getCalledFunction())
              if (syncTarget(Cee->getName()))
                syncCalls.push_back(CI);
        }

      // Redirect synchronization in EVERY function, even one that opts out of
      // access checks: a missing happens-before edge would produce a FALSE
      // POSITIVE, the one outcome the detector must never have.
      for (CallInst *CI : syncCalls) {
        Function *Cee = CI->getCalledFunction();
        const char *to = syncTarget(Cee->getName());
        // Reuse the original signature so the redirect is correct on any
        // platform (pthread_t is a pointer on macOS, an integer on Linux).
        FunctionCallee W = M.getOrInsertFunction(to, Cee->getFunctionType());
        CI->setCalledFunction(W);
        ++syncs;
      }

      // A function may opt out of ACCESS instrumentation (its sync edges are
      // still tracked above) via disable_sanitizer_instrumentation.
      if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
        continue;

      for (Instruction *I : mem) {
        // Plain (non-atomic) load/store -> a data access we check for races.
        if (auto *LI = dyn_cast<LoadInst>(I)) {
          if (!LI->isAtomic()) {
            emitAccess(LI->getPointerOperand(), LI->getType(), /*isWrite=*/false,
                       I);
            continue;
          }
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
          if (!SI->isAtomic()) {
            emitAccess(SI->getPointerOperand(), SI->getValueOperand()->getType(),
                       /*isWrite=*/true, I);
            continue;
          }
        }

        // Atomic op -> SYNCHRONIZATION, not a data race. Model it as
        // acquire/release on a per-location sync object: a read acquires AFTER
        // the op (so it sees what it observed), a write releases BEFORE it (so a
        // future reader that observes it sees our prior writes), an RMW/cmpxchg
        // does both. Hence correct lock-free code does not false-positive;
        // relaxed orderings are over-approximated (at worst a missed race).
        Value *Ptr = nullptr;
        bool reads = false, writes = false;
        if (auto *LI = dyn_cast<LoadInst>(I)) {
          Ptr = LI->getPointerOperand();
          reads = true;
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
          Ptr = SI->getPointerOperand();
          writes = true;
        } else if (auto *RMW = dyn_cast<AtomicRMWInst>(I)) {
          Ptr = RMW->getPointerOperand();
          reads = writes = true;
        } else if (auto *CX = dyn_cast<AtomicCmpXchgInst>(I)) {
          Ptr = CX->getPointerOperand();
          reads = writes = true;
        } else {
          continue;
        }
        if (writes) {
          IRBuilder<> B(I); // release before the store / RMW
          B.CreateCall(AtomicRelease, {Ptr});
        }
        if (reads) {
          IRBuilder<> B(I->getNextNode()); // acquire after the load / RMW
          B.CreateCall(AtomicAcquire, {Ptr});
        }
        ++atomics;
      }
    }

    errs() << "[redzone-race] instrumented " << accesses << " access(es) + "
           << atomics << " atomic op(s); redirected " << syncs
           << " sync call(s)\n";
    bool Changed = accesses || atomics || syncs;
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
