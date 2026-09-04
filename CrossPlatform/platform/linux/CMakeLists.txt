cmake_minimum_required(VERSION 3.16)

project(LocalFlowLinuxPlatform LANGUAGES CXX)

option(LOCALFLOW_LINUX_BUILD_TESTS "Build Linux platform unit tests" ON)

add_library(localflow_linux_platform STATIC
    src/AtSpiTextInsertion.cpp
    src/Audio.cpp
    src/Capabilities.cpp
    src/Clipboard.cpp
    src/GlobalShortcut.cpp
    src/PortalSupport.cpp
    src/Process.cpp
    src/QDbusPortals.cpp
    src/QtClipboard.cpp
    src/ScreenContext.cpp
    src/TextInsertion.cpp
    src/X11GlobalShortcut.cpp
    src/X11PasteInjector.cpp
    src/X11ScreenContext.cpp
)

target_compile_features(localflow_linux_platform PUBLIC cxx_std_17)
set_target_properties(localflow_linux_platform PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)
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

# The desktop build supplies Qt, while the standalone capability/test build is
# still useful on minimal systems. QtGui enables the full-fidelity clipboard;
# QtDBus additionally enables the XDG portal transports.
find_package(Qt6 6.4 QUIET COMPONENTS Core Gui)
if(TARGET Qt6::Core AND TARGET Qt6::Gui)
    set(LOCALFLOW_LINUX_QT_CLIPBOARD TRUE)
    target_compile_definitions(localflow_linux_platform PRIVATE
        LOCALFLOW_LINUX_WITH_QT_CLIPBOARD=1)
    target_link_libraries(localflow_linux_platform PRIVATE
        Qt6::Core Qt6::Gui)
else()
    set(LOCALFLOW_LINUX_QT_CLIPBOARD FALSE)
    target_compile_definitions(localflow_linux_platform PRIVATE
        LOCALFLOW_LINUX_WITH_QT_CLIPBOARD=0)
    message(STATUS
        "Qt6 Gui not found: clipboard preservation is limited to plain text")
endif()

if(LOCALFLOW_LINUX_QT_CLIPBOARD)
    find_package(Qt6 6.4 QUIET COMPONENTS DBus)
endif()
if(LOCALFLOW_LINUX_QT_CLIPBOARD AND TARGET Qt6::DBus)
    set(LOCALFLOW_LINUX_QT_PORTALS TRUE)
    target_compile_definitions(localflow_linux_platform PRIVATE
        LOCALFLOW_LINUX_WITH_QT_PORTALS=1)
    target_link_libraries(localflow_linux_platform PRIVATE
        Qt6::DBus)
    set_target_properties(localflow_linux_platform PROPERTIES
        AUTOMOC ON
        # GCC exposes a legacy `linux` macro to moc even though C++17 mode
        # suppresses it for the compiler; it collides with our namespace.
        AUTOMOC_MOC_OPTIONS "-Ulinux")
else()
    set(LOCALFLOW_LINUX_QT_PORTALS FALSE)
    target_compile_definitions(localflow_linux_platform PRIVATE
        LOCALFLOW_LINUX_WITH_QT_PORTALS=0)
    message(STATUS
        "Qt6 DBus not found: Wayland portal transports will report not configured")
endif()

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
    set_target_properties(localflow_linux_platform_tests PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_include_directories(localflow_linux_platform_tests PRIVATE src)
    target_link_libraries(localflow_linux_platform_tests PRIVATE localflow_linux_platform)
    add_test(NAME localflow_linux_platform_tests COMMAND localflow_linux_platform_tests)

    if(LOCALFLOW_LINUX_QT_CLIPBOARD)
        add_executable(localflow_linux_qt_clipboard_tests
            tests/QtClipboardIntegrationTests.cpp)
        set_target_properties(localflow_linux_qt_clipboard_tests PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO)
        target_compile_options(localflow_linux_qt_clipboard_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        target_link_libraries(localflow_linux_qt_clipboard_tests PRIVATE
            localflow_linux_platform Qt6::Core Qt6::Gui)
        add_test(
            NAME localflow_linux_qt_clipboard_tests
            COMMAND $<TARGET_FILE:localflow_linux_qt_clipboard_tests>)
        set_tests_properties(localflow_linux_qt_clipboard_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 15)
    endif()

    find_program(LOCALFLOW_DBUS_RUN_SESSION dbus-run-session)
    if(LOCALFLOW_LINUX_QT_PORTALS AND LOCALFLOW_DBUS_RUN_SESSION)
        add_executable(localflow_linux_qdbus_portal_tests
            tests/QDbusPortalIntegrationTests.cpp)
        set_target_properties(localflow_linux_qdbus_portal_tests PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO)
        target_compile_options(localflow_linux_qdbus_portal_tests PRIVATE
            -Wall -Wextra -Wpedantic)
        target_link_libraries(localflow_linux_qdbus_portal_tests PRIVATE
            localflow_linux_platform Qt6::Core Qt6::DBus Qt6::Gui)
        add_test(
            NAME localflow_linux_qdbus_portal_tests
            COMMAND "${LOCALFLOW_DBUS_RUN_SESSION}" --
                $<TARGET_FILE:localflow_linux_qdbus_portal_tests>)
        set_tests_properties(localflow_linux_qdbus_portal_tests PROPERTIES
            TIMEOUT 15)
    endif()
endif()
