find_package(Qt6 6.4 REQUIRED COMPONENTS Core Concurrent Gui Qml Quick QuickControls2 Widgets Network)
set(CMAKE_AUTOMOC ON)

qt_add_executable(localflow_desktop
    src/main.cpp
    src/AppController.cpp
    src/LearnedTermModel.cpp
    src/LocalOcr.cpp
    src/ModelManager.cpp
    src/ModelVerification.cpp
    src/PlatformBridge.cpp
    src/PolishWorkerClient.cpp
    src/ReplacementModel.cpp
    src/ScreenTermCaptureCoordinator.cpp
    src/SettingsModel.cpp
    src/SingleInstance.cpp
    src/UpdateManager.cpp
)

qt_add_resources(localflow_desktop "localflow_assets"
    PREFIX "/LocalFlow"
    FILES
        assets/LocalFlow.png
)

if(BUILD_TESTING)
    add_executable(localflow_learned_term_model_tests
        src/LearnedTermModel.cpp
        src/LearnedTermModelTests.cpp)
    set_target_properties(localflow_learned_term_model_tests PROPERTIES AUTOMOC ON)
    target_compile_features(localflow_learned_term_model_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_learned_term_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(localflow_learned_term_model_tests PRIVATE Qt6::Core LocalFlow::Core)
    add_test(NAME localflow_learned_term_model_tests COMMAND localflow_learned_term_model_tests)

    add_executable(localflow_update_manager_tests
        src/UpdateManager.cpp
        tests/UpdateManagerTests.cpp)
    set_target_properties(localflow_update_manager_tests PROPERTIES AUTOMOC ON)
    target_compile_features(localflow_update_manager_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_update_manager_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(localflow_update_manager_tests PRIVATE
        Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Network)
    target_compile_definitions(localflow_update_manager_tests PRIVATE
        LOCALFLOW_VERSION="1.2.3"
        LOCALFLOW_WINDOWS_SIGNER_SHA256=""
        LOCALFLOW_UPDATE_PUBLIC_ED_KEY="11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=")
    if(WIN32)
        target_link_libraries(localflow_update_manager_tests PRIVATE
            LocalFlow::UpdateEd25519)
    endif()
    add_test(NAME localflow_update_manager_tests COMMAND localflow_update_manager_tests)

    add_executable(localflow_screen_term_capture_tests
        src/ScreenTermCaptureCoordinator.cpp
        tests/ScreenTermCaptureCoordinatorTests.cpp)
    target_compile_features(localflow_screen_term_capture_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_screen_term_capture_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME localflow_screen_term_capture_tests
        COMMAND localflow_screen_term_capture_tests)

    add_executable(localflow_replacement_model_tests
        src/ReplacementModel.cpp
        tests/ReplacementModelTests.cpp)
    set_target_properties(localflow_replacement_model_tests PROPERTIES AUTOMOC ON)
    target_compile_features(localflow_replacement_model_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_replacement_model_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(localflow_replacement_model_tests PRIVATE Qt6::Core)
    add_test(NAME localflow_replacement_model_tests
        COMMAND localflow_replacement_model_tests)

    add_executable(localflow_model_verification_tests
        src/ModelVerification.cpp
        tests/ModelVerificationTests.cpp)
    target_compile_features(localflow_model_verification_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_model_verification_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(localflow_model_verification_tests PRIVATE Qt6::Core)
    add_test(NAME localflow_model_verification_tests
        COMMAND localflow_model_verification_tests)

    add_executable(localflow_single_instance_tests
        src/SingleInstance.cpp
        tests/SingleInstanceTests.cpp)
    target_compile_features(localflow_single_instance_tests PRIVATE cxx_std_17)
    target_include_directories(localflow_single_instance_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(localflow_single_instance_tests PRIVATE
        Qt6::Core Qt6::Network)
    add_test(NAME localflow_single_instance_tests
        COMMAND localflow_single_instance_tests)
endif()

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
    Qt6::Widgets Qt6::Network
    LocalFlow::Core LocalFlow::ASR LocalFlow::Platform
)
if(WIN32)
    target_sources(localflow_desktop PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/LocalFlow.rc")
    target_link_libraries(localflow_desktop PRIVATE
        crypt32 version wintrust LocalFlow::UpdateEd25519)
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
    if(LOCALFLOW_REQUIRE_OCR AND NOT LOCALFLOW_HAVE_TESSERACT)
        message(FATAL_ERROR
            "LOCALFLOW_REQUIRE_OCR is enabled, but Tesseract was not found. "
            "Install libtesseract-dev before producing a Linux release.")
    endif()
endif()
target_compile_definitions(localflow_desktop PRIVATE
    LOCALFLOW_HAVE_TESSERACT=${LOCALFLOW_HAVE_TESSERACT})
if(WIN32)
    string(LENGTH "${LOCALFLOW_UPDATE_PUBLIC_ED_KEY}" LOCALFLOW_UPDATE_PUBLIC_ED_KEY_LENGTH)
    if(LOCALFLOW_REQUIRE_SIGNED_UPDATES AND
       (NOT LOCALFLOW_UPDATE_PUBLIC_ED_KEY_LENGTH EQUAL 44 OR
        NOT LOCALFLOW_UPDATE_PUBLIC_ED_KEY MATCHES "^[A-Za-z0-9+/]+=$"))
        message(FATAL_ERROR
            "Production Windows builds require a canonical base64 Ed25519 release-verification key.")
    endif()
    string(LENGTH "${LOCALFLOW_WINDOWS_SIGNER_SHA256}" LOCALFLOW_WINDOWS_SIGNER_LENGTH)
    if(LOCALFLOW_REQUIRE_AUTHENTICODE_UPDATES AND
       (NOT LOCALFLOW_WINDOWS_SIGNER_LENGTH EQUAL 64 OR
        NOT LOCALFLOW_WINDOWS_SIGNER_SHA256 MATCHES "^[0-9A-Fa-f]+$"))
        message(FATAL_ERROR
            "Authenticated Windows builds require LOCALFLOW_WINDOWS_SIGNER_SHA256 as 64 hexadecimal characters.")
    endif()
    target_compile_definitions(localflow_desktop PRIVATE
        LOCALFLOW_WINDOWS_SIGNER_SHA256="${LOCALFLOW_WINDOWS_SIGNER_SHA256}"
        LOCALFLOW_UPDATE_PUBLIC_ED_KEY="${LOCALFLOW_UPDATE_PUBLIC_ED_KEY}")
endif()

set_target_properties(localflow_desktop PROPERTIES
    OUTPUT_NAME "LocalFlow"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
)

if(UNIX AND NOT APPLE)
    set_target_properties(localflow_desktop PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib/localflow/asr")
endif()

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
