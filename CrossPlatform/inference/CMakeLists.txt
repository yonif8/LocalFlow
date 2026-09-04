add_library(localflow_asr STATIC src/NemoTranscriber.cpp)
add_library(LocalFlow::ASR ALIAS localflow_asr)
target_compile_features(localflow_asr PUBLIC cxx_std_17)
target_include_directories(localflow_asr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(localflow_polish STATIC src/S1MiniPolisher.cpp)
add_library(LocalFlow::Polish ALIAS localflow_polish)
target_compile_features(localflow_polish PUBLIC cxx_std_17)
target_include_directories(localflow_polish PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Convenience aggregate for tests and tools that can load both runtimes in one
# process. The desktop links ASR only: pinned NeMo and current llama.cpp builds
# use incompatible ggml ABIs, so S1 runs in a persistent helper process.
add_library(localflow_inference INTERFACE)
add_library(LocalFlow::Inference ALIAS localflow_inference)
target_link_libraries(localflow_inference INTERFACE LocalFlow::ASR LocalFlow::Polish)

option(LOCALFLOW_ENABLE_NEMO_SPEECH "Enable NeMo-Speech.cpp ASR" ON)
option(LOCALFLOW_ENABLE_LLAMA "Enable llama.cpp S1-mini polish" ON)

if(LOCALFLOW_REQUIRE_INFERENCE AND (NOT LOCALFLOW_ENABLE_NEMO_SPEECH OR NOT LOCALFLOW_ENABLE_LLAMA))
    message(FATAL_ERROR "LOCALFLOW_REQUIRE_INFERENCE requires both inference backends to be enabled")
endif()

set(LOCALFLOW_RUNTIME_FIND_POLICY)
if(LOCALFLOW_REQUIRE_INFERENCE)
    list(APPEND LOCALFLOW_RUNTIME_FIND_POLICY NO_DEFAULT_PATH)
endif()

if(LOCALFLOW_ENABLE_NEMO_SPEECH)
    find_path(NEMO_SPEECH_INCLUDE_DIR nemo_speech/asr.h
        HINTS "${NEMO_SPEECH_ROOT}/include" "$ENV{NEMO_SPEECH_ROOT}/include"
        ${LOCALFLOW_RUNTIME_FIND_POLICY})
    find_library(NEMO_SPEECH_ASR_LIBRARY
        NAMES nemo_speech_asr_c nemo_speech_asr nemo-speech-asr
        HINTS
            "${NEMO_SPEECH_ROOT}/lib" "${NEMO_SPEECH_ROOT}/bin"
            "$ENV{NEMO_SPEECH_ROOT}/lib" "$ENV{NEMO_SPEECH_ROOT}/bin"
        ${LOCALFLOW_RUNTIME_FIND_POLICY})
endif()

if(LOCALFLOW_ENABLE_NEMO_SPEECH AND NEMO_SPEECH_INCLUDE_DIR AND NEMO_SPEECH_ASR_LIBRARY)
    target_compile_definitions(localflow_asr PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=1)
    target_include_directories(localflow_asr PRIVATE "${NEMO_SPEECH_INCLUDE_DIR}")
    target_link_libraries(localflow_asr PRIVATE "${NEMO_SPEECH_ASR_LIBRARY}")
else()
    if(LOCALFLOW_REQUIRE_INFERENCE)
        message(FATAL_ERROR "NeMo-Speech.cpp is required but its headers or ASR import library were not found")
    endif()
    target_compile_definitions(localflow_asr PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=0)
    message(STATUS "NeMo-Speech.cpp not found; ASR adapter will report unavailable")
endif()

if(LOCALFLOW_ENABLE_LLAMA)
    find_path(LLAMA_INCLUDE_DIR llama.h
        HINTS "${LLAMA_ROOT}/include" "$ENV{LLAMA_ROOT}/include"
        ${LOCALFLOW_RUNTIME_FIND_POLICY})
endif()

if(LOCALFLOW_ENABLE_LLAMA AND LLAMA_INCLUDE_DIR)
    target_compile_definitions(localflow_polish PRIVATE LOCALFLOW_HAVE_LLAMA=1)
    target_include_directories(localflow_polish PRIVATE "${LLAMA_INCLUDE_DIR}")
    target_link_libraries(localflow_polish PRIVATE "${CMAKE_DL_LIBS}")
else()
    if(LOCALFLOW_REQUIRE_INFERENCE)
        message(FATAL_ERROR "llama.cpp is required but its pinned headers were not found")
    endif()
    target_compile_definitions(localflow_polish PRIVATE LOCALFLOW_HAVE_LLAMA=0)
    message(STATUS "llama.cpp headers not found; S1-mini adapter will report unavailable")
endif()

if(BUILD_TESTING)
    # Keep the two incompatible ggml ABIs in separate test processes just as
    # they are in the shipped app and persistent polish worker.
    add_executable(localflow_polish_contract_tests tests/InferenceTests.cpp)
    target_link_libraries(localflow_polish_contract_tests PRIVATE LocalFlow::Polish)
    add_test(NAME localflow.inference.polish-contracts COMMAND localflow_polish_contract_tests)

    add_executable(localflow_asr_contract_tests tests/NemoContractTests.cpp)
    target_link_libraries(localflow_asr_contract_tests PRIVATE LocalFlow::ASR)
    add_test(NAME localflow.inference.asr-contracts COMMAND localflow_asr_contract_tests)
    if(WIN32 AND NEMO_SPEECH_INCLUDE_DIR AND NEMO_SPEECH_ASR_LIBRARY)
        find_file(NEMO_SPEECH_ASR_DLL NAMES nemo_speech_asr_c.dll
            HINTS "${NEMO_SPEECH_ROOT}/bin" "$ENV{NEMO_SPEECH_ROOT}/bin")
        if(LOCALFLOW_REQUIRE_INFERENCE AND NOT NEMO_SPEECH_ASR_DLL)
            message(FATAL_ERROR "The NeMo ASR runtime DLL is required for Windows tests")
        endif()
        if(NEMO_SPEECH_ASR_DLL)
            get_filename_component(NEMO_SPEECH_RUNTIME_DIR "${NEMO_SPEECH_ASR_DLL}" DIRECTORY)
            set_tests_properties(localflow.inference.asr-contracts PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${NEMO_SPEECH_RUNTIME_DIR}")
        endif()
    endif()
endif()
