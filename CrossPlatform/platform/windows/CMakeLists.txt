cmake_minimum_required(VERSION 3.20)

project(LocalFlowWindowsAdapter LANGUAGES CXX)

option(LOCALFLOW_WINDOWS_BUILD_TESTS "Build Windows adapter tests" ON)

# The hold-state logic deliberately has no Win32 dependency, allowing its edge
# cases to run in every CI job rather than only on a Windows runner.
add_library(localflow_windows_state STATIC
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
        src/ForegroundWindow.cpp
        src/InputMonitor.cpp
        src/ScreenCapture.cpp
        src/SystemAudioDucker.cpp
        src/TextInserter.cpp
        src/WinError.cpp
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
            propsys
            user32
            uuid
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
endif()
