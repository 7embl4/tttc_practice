cmake_minimum_required(VERSION 3.20)
project(TypesAnalyzer)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})

include(AddLLVM)

add_llvm_library(TypesAnalyzer MODULE types_analyzer.cpp PLUGIN_TOOL clang)
