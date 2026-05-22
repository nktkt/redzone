# Minimal example of using redzone from CMake.
#
# Build (from the redzone repo root), pointing at the Homebrew Clang:
#   cmake -S integration/cmake-example -B /tmp/rz-cmake \
#         -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang
#   cmake --build /tmp/rz-cmake
#   /tmp/rz-cmake/demo          # detects a heap-buffer-overflow, exits nonzero
cmake_minimum_required(VERSION 3.20)
project(redzone_cmake_example C)

# Locate the redzone checkout. Inside this repo it's two levels up; pass
# -DREDZONE_ROOT=/path/to/redzone when using this as a standalone template.
if(NOT DEFINED REDZONE_ROOT)
  get_filename_component(REDZONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
include(${REDZONE_ROOT}/cmake/Redzone.cmake)

add_executable(demo main.c)
redzone_enable(demo)
