cmake_minimum_required(VERSION 3.25)

project(ckutilities_ckvision_package_consumer LANGUAGES CXX)

find_package(ckvision CONFIG REQUIRED)

add_executable(ckvision_package_consumer main.cpp)
target_compile_features(ckvision_package_consumer PRIVATE cxx_std_20)
target_link_libraries(ckvision_package_consumer PRIVATE ckvision::cvision)

enable_testing()
add_test(NAME ckvision_package_consumer_runs COMMAND ckvision_package_consumer)
