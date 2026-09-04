cmake_minimum_required(VERSION 3.16)

project(LocalFlowLinuxPlatform LANGUAGES CXX)

option(LOCALFLOW_LINUX_BUILD_TESTS "Build Linux platform unit tests" ON)

add_library(localflow_linux_platform STATIC
    src/AtSpiTextInsertion.cpp
    src/Audio.cpp
    src/Capabilities.cpp
    src/Clipboard.cpp
    src/GlobalShortcut.cpp
    src/Process.cpp
    src/ScreenContext.cpp
    src/TextInsertion.cpp
    src/X11GlobalShortcut.cpp
    src/X11PasteInjector.cpp
    src/X11ScreenContext.cpp
)

target_compile_features(localflow_linux_platform PUBLIC cxx_std_17)
target_include_directories(localflow_linux_platform
    PUBLIC include
    PRIVATE src
)
target_compile_options(localflow_linux_platform PRIVATE
    -Wall -Wextra -Wpedantic
)

find_package(Threads REQUIRED)
target_link_libraries(localflow_linux_platform PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Native integrations are optional at build time. The library still builds and
# returns actionable missing-dependency statuses when a development package is
# absent; runtime capability detection is a separate check.
find_package(X11 QUIET)
if(X11_FOUND)
    target_compile_definitions(localflow_linux_platform PRIVATE LOCALFLOW_LINUX_WITH_X11=1)
    target_include_directories(localflow_linux_platform PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(localflow_linux_platform PRIVATE ${X11_LIBRARIES})

    find_library(LOCALFLOW_XTST_LIBRARY Xtst)
    if(LOCALFLOW_XTST_LIBRARY)
        target_compile_definitions(localflow_linux_platform PRIVATE LOCALFLOW_LINUX_WITH_XTEST=1)
        target_link_libraries(localflow_linux_platform PRIVATE ${LOCALFLOW_XTST_LIBRARY})
    endif()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ATSPI QUIET IMPORTED_TARGET atspi-2 gobject-2.0)
    if(ATSPI_FOUND)
        target_compile_definitions(localflow_linux_platform PRIVATE LOCALFLOW_LINUX_WITH_ATSPI=1)
        target_link_libraries(localflow_linux_platform PRIVATE PkgConfig::ATSPI)
    endif()
endif()

if(LOCALFLOW_LINUX_BUILD_TESTS)
    enable_testing()
    add_executable(localflow_linux_platform_tests tests/LinuxPlatformTests.cpp)
    target_compile_features(localflow_linux_platform_tests PRIVATE cxx_std_17)
    target_link_libraries(localflow_linux_platform_tests PRIVATE localflow_linux_platform)
    add_test(NAME localflow_linux_platform_tests COMMAND localflow_linux_platform_tests)
endif()
