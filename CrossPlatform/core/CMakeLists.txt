cmake_minimum_required(VERSION 3.20)

project(LocalFlowCore LANGUAGES CXX)

add_library(localflow_core STATIC
    src/learned_terminology.cpp
    src/replacements.cpp
    src/terminology.cpp
    src/text_chunker.cpp
    src/utf8.cpp
)
add_library(LocalFlow::Core ALIAS localflow_core)

target_compile_features(localflow_core PUBLIC cxx_std_17)
target_include_directories(localflow_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(localflow_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(localflow_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(LOCALFLOW_BUILD_CORE_TESTS "Build the dependency-free core tests" OFF)
if(LOCALFLOW_BUILD_CORE_TESTS)
    enable_testing()
    add_subdirectory(../tests/core ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()

