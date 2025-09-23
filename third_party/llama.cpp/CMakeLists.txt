cmake_minimum_required(VERSION 3.16)

project(llama_cpp_stub LANGUAGES CXX)

add_library(llama_cpp STATIC
  llama_stub.cpp
)

target_include_directories(llama_cpp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(llama_cpp PUBLIC cxx_std_20)

install(TARGETS llama_cpp
  ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
