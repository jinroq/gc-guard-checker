cmake_minimum_required(VERSION 3.20)
project(GCGuardCheck CXX)

find_package(LLVM 21.1 REQUIRED CONFIG HINTS /usr/lib/llvm-21/lib/cmake/llvm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(GCGuardCheck MODULE src/GCGuardCheck.cpp)
set_target_properties(GCGuardCheck PROPERTIES PREFIX "")
target_include_directories(GCGuardCheck SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(GCGuardCheck PRIVATE ${LLVM_DEFINITIONS})
target_compile_options(GCGuardCheck PRIVATE -fno-rtti -Wall -Wextra)
