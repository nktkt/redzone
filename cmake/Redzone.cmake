# Redzone.cmake -- drop-in integration for the redzone memory-safety detector.
#
# Usage in your CMakeLists.txt:
#
#   set(REDZONE_ROOT /path/to/redzone)          # a redzone checkout
#   include(${REDZONE_ROOT}/cmake/Redzone.cmake)
#   add_executable(myprog main.c)
#   redzone_enable(myprog)                       # instrument + link the runtime
#
# Requirements:
#   * The target must be built with the *Homebrew/upstream* Clang whose LLVM
#     matches the one the pass plugin was built against (Apple Clang cannot load
#     it). Configure with, e.g.:
#         -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang
#   * The pass plugin must be built at ${REDZONE_ROOT}/build/libRedzonePass.so:
#         cmake -S ${REDZONE_ROOT} -B ${REDZONE_ROOT}/build \
#               -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
#         cmake --build ${REDZONE_ROOT}/build

# Default REDZONE_ROOT to the checkout this module lives in (<root>/cmake/..).
if(NOT DEFINED REDZONE_ROOT)
  get_filename_component(REDZONE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(REDZONE_PLUGIN "${REDZONE_ROOT}/build/libRedzonePass.so"
    CACHE FILEPATH "Path to the redzone pass plugin")
set(REDZONE_RUNTIME "${REDZONE_ROOT}/runtime/redzone_rt.c"
    CACHE FILEPATH "Path to the redzone runtime source")

# The runtime, compiled WITHOUT instrumentation, shared by all enabled targets.
# (Instrumenting it would rewrite its own malloc and recurse.) Guard against
# being included more than once.
if(NOT TARGET redzone_runtime)
  add_library(redzone_runtime OBJECT "${REDZONE_RUNTIME}")
endif()

# redzone_enable(<target>): run the redzone pass on the target's sources during
# compilation, and link in the runtime.
function(redzone_enable target)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "redzone: target '${target}' must be built with upstream Clang "
      "(found '${CMAKE_C_COMPILER_ID}'). Configure with "
      "-DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang")
  endif()
  if(NOT EXISTS "${REDZONE_PLUGIN}")
    message(FATAL_ERROR
      "redzone: pass plugin not found at ${REDZONE_PLUGIN}. Build it first:\n"
      "  cmake -S ${REDZONE_ROOT} -B ${REDZONE_ROOT}/build "
      "-DCMAKE_PREFIX_PATH=$(brew --prefix llvm) && "
      "cmake --build ${REDZONE_ROOT}/build")
  endif()
  target_compile_options(${target} PRIVATE -g "-fpass-plugin=${REDZONE_PLUGIN}")
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:redzone_runtime>)
endfunction()
