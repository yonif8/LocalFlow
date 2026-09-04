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

if(LOCALFLOW_ENABLE_NEMO_SPEECH)
    find_path(NEMO_SPEECH_INCLUDE_DIR nemo_speech/asr.h
        HINTS "${NEMO_SPEECH_ROOT}/include")
    find_library(NEMO_SPEECH_ASR_LIBRARY
        NAMES nemo_speech_asr_c nemo_speech_asr nemo-speech-asr
        HINTS "${NEMO_SPEECH_ROOT}/lib" "${NEMO_SPEECH_ROOT}/bin")
endif()

if(NEMO_SPEECH_INCLUDE_DIR AND NEMO_SPEECH_ASR_LIBRARY)
    target_compile_definitions(localflow_asr PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=1)
    target_include_directories(localflow_asr PRIVATE "${NEMO_SPEECH_INCLUDE_DIR}")
    target_link_libraries(localflow_asr PRIVATE "${NEMO_SPEECH_ASR_LIBRARY}")
else()
    target_compile_definitions(localflow_asr PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=0)
    message(STATUS "NeMo-Speech.cpp not found; ASR adapter will report unavailable")
endif()

if(LOCALFLOW_ENABLE_LLAMA)
    find_path(LLAMA_INCLUDE_DIR llama.h HINTS "${LLAMA_ROOT}/include")
    find_library(LLAMA_LIBRARY NAMES llama HINTS "${LLAMA_ROOT}/lib" "${LLAMA_ROOT}/bin")
endif()

if(LLAMA_INCLUDE_DIR AND LLAMA_LIBRARY)
    target_compile_definitions(localflow_polish PRIVATE LOCALFLOW_HAVE_LLAMA=1)
    target_include_directories(localflow_polish PRIVATE "${LLAMA_INCLUDE_DIR}")
    target_link_libraries(localflow_polish PRIVATE "${LLAMA_LIBRARY}")
else()
    target_compile_definitions(localflow_polish PRIVATE LOCALFLOW_HAVE_LLAMA=0)
    message(STATUS "llama.cpp not found; S1-mini adapter will report unavailable")
endif()

if(BUILD_TESTING)
    add_executable(localflow_inference_tests tests/InferenceTests.cpp)
    target_link_libraries(localflow_inference_tests PRIVATE LocalFlow::Inference)
    add_test(NAME localflow.inference.contracts COMMAND localflow_inference_tests)
endif()
