include(FetchContent)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core)

# Use the exact portable Ed25519 implementation already reviewed and shipped
# by the pinned Sparkle dependency. URL_HASH protects CMake builds from a
# replaced archive, while the immutable commit matches Package.resolved.
FetchContent_Declare(localflow_sparkle_ed25519_source
    URL
        "https://codeload.github.com/sparkle-project/Sparkle/tar.gz/ac2def288cbff5cfc7df3ffef6abdf45b72bcb0a"
    URL_HASH
        "SHA256=49c528efd86801a378ba7196da3ac9722a1165fbf4813ed76c9d636ad054fb9e"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(localflow_sparkle_ed25519_source)

set(LOCALFLOW_ED25519_SOURCE_DIR
    "${localflow_sparkle_ed25519_source_SOURCE_DIR}/Vendor/ed25519-sparkle/src")
foreach(required_source
        ed25519.h fe.c fe.h fixedint.h ge.c ge.h keypair.c precomp_data.h
        sc.c sc.h sha512.c sha512.h sign.c verify.c)
    if(NOT EXISTS "${LOCALFLOW_ED25519_SOURCE_DIR}/${required_source}")
        message(FATAL_ERROR
            "The checksum-pinned Sparkle archive is missing ${required_source}.")
    endif()
endforeach()

add_library(localflow_update_ed25519 STATIC
    "${LOCALFLOW_ED25519_SOURCE_DIR}/fe.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/ge.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/keypair.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/sc.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/sha512.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/sign.c"
    "${LOCALFLOW_ED25519_SOURCE_DIR}/verify.c")
add_library(LocalFlow::UpdateEd25519 ALIAS localflow_update_ed25519)
target_include_directories(localflow_update_ed25519 PUBLIC
    "${LOCALFLOW_ED25519_SOURCE_DIR}")
target_compile_definitions(localflow_update_ed25519 PRIVATE ED25519_NO_SEED)

add_executable(localflow_update_signer update_signer_main.cpp)
target_compile_features(localflow_update_signer PRIVATE cxx_std_17)
target_link_libraries(localflow_update_signer PRIVATE
    Qt6::Core LocalFlow::UpdateEd25519)
target_compile_definitions(localflow_update_signer PRIVATE
    LOCALFLOW_UPDATE_PUBLIC_ED_KEY="${LOCALFLOW_UPDATE_PUBLIC_ED_KEY}")
set_target_properties(localflow_update_signer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(BUILD_TESTING)
    add_executable(localflow_update_ed25519_tests ed25519_tests.cpp)
    target_compile_features(localflow_update_ed25519_tests PRIVATE cxx_std_17)
    target_link_libraries(localflow_update_ed25519_tests PRIVATE
        LocalFlow::UpdateEd25519)
    add_test(NAME localflow_update_ed25519_tests
        COMMAND localflow_update_ed25519_tests)
endif()
