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
option(LOCALFLOW_REQUIRE_INFERENCE "Fail configure unless both pinned inference runtimes are available" OFF)
option(LOCALFLOW_REQUIRE_OCR "Fail configure unless the Linux local OCR runtime is available" OFF)
option(LOCALFLOW_REQUIRE_SIGNED_UPDATES "Fail configure unless the Windows update signer is pinned" OFF)
set(LOCALFLOW_WINDOWS_SIGNER_SHA256 "" CACHE STRING "Trusted Windows update signer certificate SHA-256")

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
    foreach(target localflow_core localflow_asr localflow_polish localflow_desktop localflow_polish_worker
                   localflow_polish_contract_tests localflow_asr_contract_tests
                   localflow_learned_term_model_tests localflow_update_manager_tests
                   localflow_screen_term_capture_tests localflow_replacement_model_tests
                   localflow_model_verification_tests
                   localflow_single_instance_tests
                   localflow_core_tests
                   localflow_linux_platform localflow_linux_platform_tests
                   localflow_linux_qt_clipboard_tests localflow_linux_qdbus_portal_tests
                   localflow_windows localflow_windows_state
                   localflow_windows_state_tests localflow_windows_ocr_text_tests)
        if(TARGET ${target})
            if(MSVC)
                target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /WX)
            else()
                target_compile_options(${target} PRIVATE
                    -Wall -Wextra -Wpedantic -Werror)
            endif()
        endif()
    endforeach()
endif()
