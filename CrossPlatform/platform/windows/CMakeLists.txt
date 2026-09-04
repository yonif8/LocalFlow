cmake_minimum_required(VERSION 3.20)

project(LocalFlowWindowsAdapter LANGUAGES CXX)

option(LOCALFLOW_WINDOWS_BUILD_TESTS "Build Windows adapter tests" ON)

# The transition and safety policies deliberately have no Win32 dependency,
# allowing their edge cases to run in every CI job, not only Windows runners.
add_library(localflow_windows_state STATIC
    src/AudioSafetyState.cpp
    src/FocusedTextTargetState.cpp
    src/OcrTextNormalization.cpp
    src/PttStateMachine.cpp
)
add_library(LocalFlow::WindowsState ALIAS localflow_windows_state)
target_compile_features(localflow_windows_state PUBLIC cxx_std_17)
target_include_directories(localflow_windows_state
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(WIN32)
    add_library(localflow_windows STATIC
        src/AudioCapture.cpp
        src/ClipboardTransaction.cpp
        src/FocusedTextTarget.cpp
        src/ForegroundWindow.cpp
        src/InputMonitor.cpp
        src/ScreenCapture.cpp
        src/SystemAudioDucker.cpp
        src/TextInserter.cpp
        src/WinError.cpp
        src/WindowsMediaOcr.cpp
    )
    add_library(LocalFlow::Windows ALIAS localflow_windows)
    target_compile_features(localflow_windows PUBLIC cxx_std_17)
    target_include_directories(localflow_windows
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_definitions(localflow_windows
        PUBLIC
            UNICODE
            _UNICODE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(localflow_windows
        PUBLIC
            LocalFlow::WindowsState
        PRIVATE
            avrt
            gdi32
            ole32
            oleaut32
            propsys
            user32
            uuid
            windowsapp
    )
    if(MSVC)
        target_compile_options(localflow_windows PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_compile_options(localflow_windows_state PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(localflow_windows PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(localflow_windows_state PRIVATE -Wall -Wextra -Wpedantic)
    endif()
else()
    message(STATUS "Non-Windows host: building portable Windows adapter state/tests only")
    target_compile_options(localflow_windows_state PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LOCALFLOW_WINDOWS_BUILD_TESTS)
    include(CTest)
    add_executable(localflow_windows_state_tests tests/PttStateMachineTests.cpp)
    target_compile_features(localflow_windows_state_tests PRIVATE cxx_std_17)
    target_link_libraries(localflow_windows_state_tests PRIVATE LocalFlow::WindowsState)
    add_test(NAME localflow.windows.ptt_state COMMAND localflow_windows_state_tests)

    add_executable(localflow_windows_ocr_text_tests tests/OcrTextNormalizationTests.cpp)
    target_compile_features(localflow_windows_ocr_text_tests PRIVATE cxx_std_17)
    target_link_libraries(localflow_windows_ocr_text_tests PRIVATE LocalFlow::WindowsState)
    add_test(NAME localflow.windows.ocr_text_normalization COMMAND localflow_windows_ocr_text_tests)

    add_executable(localflow_windows_safety_state_tests tests/SafetyStateTests.cpp)
    target_compile_features(localflow_windows_safety_state_tests PRIVATE cxx_std_17)
    target_link_libraries(localflow_windows_safety_state_tests PRIVATE LocalFlow::WindowsState)
    if(MSVC)
        target_compile_options(localflow_windows_safety_state_tests PRIVATE
            /W4 /permissive- /EHsc /utf-8)
        if(LOCALFLOW_STRICT_WARNINGS)
            target_compile_options(localflow_windows_safety_state_tests PRIVATE /WX)
        endif()
    else()
        target_compile_options(localflow_windows_safety_state_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        if(LOCALFLOW_STRICT_WARNINGS)
            target_compile_options(localflow_windows_safety_state_tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME localflow.windows.safety_state COMMAND localflow_windows_safety_state_tests)
endif()
