cmake_minimum_required(VERSION 3.26)

project(LocalFlowDesktop VERSION 1.3.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LOCALFLOW_VERSION_STRING "1.3.0-beta.1" CACHE STRING "User-visible LocalFlow version")
option(LOCALFLOW_BUILD_APP "Build the Windows/Linux desktop application" ON)
option(LOCALFLOW_STRICT_WARNINGS "Treat LocalFlow compiler warnings as errors" OFF)

add_subdirectory(core)
add_subdirectory(inference)

add_library(localflow_platform INTERFACE)
add_library(LocalFlow::Platform ALIAS localflow_platform)

if(WIN32)
    add_subdirectory(platform/windows)
    target_link_libraries(localflow_platform INTERFACE LocalFlow::Windows)
elseif(UNIX AND NOT APPLE)
    add_subdirectory(platform/linux)
    target_link_libraries(localflow_platform INTERFACE localflow_linux_platform)
elseif(LOCALFLOW_BUILD_APP)
    message(FATAL_ERROR "The cross-platform desktop target supports Windows and Linux. Use the Swift package for macOS.")
endif()

if(LOCALFLOW_BUILD_APP)
    add_subdirectory(app)
    target_compile_definitions(localflow_desktop PRIVATE
        LOCALFLOW_VERSION="${LOCALFLOW_VERSION_STRING}")
    target_compile_definitions(localflow_polish_worker PRIVATE
        LOCALFLOW_VERSION="${LOCALFLOW_VERSION_STRING}")
endif()

if(LOCALFLOW_STRICT_WARNINGS)
    foreach(target localflow_core localflow_asr localflow_polish localflow_desktop localflow_polish_worker)
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE /WX)
            else()
                target_compile_options(${target} PRIVATE -Werror)
            endif()
        endif()
    endforeach()
endif()
