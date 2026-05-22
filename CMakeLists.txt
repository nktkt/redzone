cmake_minimum_required(VERSION 3.20)
project(redzone LANGUAGES C CXX)

# Locate an LLVM install. Point cmake at it with:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=$(brew --prefix llvm)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})

# The pass is built as a loadable plugin (a MODULE library).
add_library(RedzonePass MODULE src/RedzonePass.cpp)

# Match LLVM's RTTI setting, or the plugin won't load cleanly.
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(RedzonePass PRIVATE -fno-rtti)
endif()

# On macOS, plugin symbols are resolved by the host (opt/clang) at load time.
if(APPLE)
  set_target_properties(RedzonePass PROPERTIES
    LINK_FLAGS "-undefined dynamic_lookup")
endif()
