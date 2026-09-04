cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(LocalFlowCoreTests LANGUAGES CXX)
    enable_testing()
endif()

if(NOT TARGET localflow_core)
    add_subdirectory(../../core ${CMAKE_CURRENT_BINARY_DIR}/localflow-core)
endif()

add_executable(localflow_core_tests core_tests.cpp)
target_link_libraries(localflow_core_tests PRIVATE LocalFlow::Core)
target_compile_features(localflow_core_tests PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(localflow_core_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(localflow_core_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME localflow_core_tests COMMAND localflow_core_tests)

