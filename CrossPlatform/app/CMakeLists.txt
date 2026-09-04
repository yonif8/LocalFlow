find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent Gui Qml Quick QuickControls2 Widgets Multimedia Network)
set(CMAKE_AUTOMOC ON)

qt_add_executable(localflow_desktop
    src/main.cpp
    src/AppController.cpp
    src/LocalOcr.cpp
    src/ModelManager.cpp
    src/PlatformBridge.cpp
    src/PolishWorkerClient.cpp
    src/SettingsModel.cpp
)

add_executable(localflow_polish_worker src/polish_worker_main.cpp)
set_target_properties(localflow_polish_worker PROPERTIES OUTPUT_NAME "localflow-polish-worker")
target_compile_features(localflow_polish_worker PRIVATE cxx_std_17)
target_link_libraries(localflow_polish_worker PRIVATE Qt6::Core LocalFlow::Polish)

qt_add_qml_module(localflow_desktop
    URI LocalFlow
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/HudWindow.qml
        qml/OnboardingWindow.qml
        qml/SettingsWindow.qml
)

target_compile_features(localflow_desktop PRIVATE cxx_std_17)
target_include_directories(localflow_desktop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(localflow_desktop PRIVATE
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::Widgets Qt6::Multimedia Qt6::Network
    LocalFlow::Core LocalFlow::ASR LocalFlow::Platform
)
if(WIN32)
    target_link_libraries(localflow_desktop PRIVATE crypt32 version wintrust)
    if(TARGET localflow_update_manager_tests)
        target_link_libraries(localflow_update_manager_tests PRIVATE
            crypt32 version wintrust)
    endif()
endif()

set(LOCALFLOW_HAVE_TESSERACT 0)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TESSERACT QUIET IMPORTED_TARGET tesseract)
        if(TESSERACT_FOUND)
            set(LOCALFLOW_HAVE_TESSERACT 1)
            target_link_libraries(localflow_desktop PRIVATE PkgConfig::TESSERACT)
        endif()
    endif()
endif()
target_compile_definitions(localflow_desktop PRIVATE
    LOCALFLOW_HAVE_TESSERACT=${LOCALFLOW_HAVE_TESSERACT})

set_target_properties(localflow_desktop PROPERTIES
    OUTPUT_NAME "LocalFlow"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
)

set_target_properties(localflow_polish_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS localflow_desktop
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)

if(WIN32)
    install(TARGETS localflow_polish_worker RUNTIME DESTINATION bin/polish)
else()
    install(TARGETS localflow_polish_worker RUNTIME DESTINATION libexec/localflow)
endif()
