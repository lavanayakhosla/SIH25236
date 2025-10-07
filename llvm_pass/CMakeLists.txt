cmake_minimum_required(VERSION 3.13)
project(llvm_obfuscator_pass)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_library(obfpass MODULE ObfuscationPass.cpp)
set_target_properties(obfpass PROPERTIES
  COMPILE_FLAGS "${LLVM_COMPILE_FLAGS}"
  PREFIX ""
)

llvm_map_components_to_libnames(REQ_LIBS
  support core irreader passes nativecodegen ipo
)
target_link_libraries(obfpass PRIVATE ${REQ_LIBS})
