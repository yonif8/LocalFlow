add_library(localflow_inference
    src/NemoTranscriber.cpp
    src/S1MiniPolisher.cpp
)
add_library(LocalFlow::Inference ALIAS localflow_inference)

target_compile_features(localflow_inference PUBLIC cxx_std_17)
target_include_directories(localflow_inference
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

option(LOCALFLOW_ENABLE_NEMO_SPEECH "Enable NeMo-Speech.cpp ASR" ON)
option(LOCALFLOW_ENABLE_LLAMA "Enable llama.cpp S1-mini polish" ON)

if(LOCALFLOW_ENABLE_NEMO_SPEECH)
    find_path(NEMO_SPEECH_INCLUDE_DIR nemo_speech/asr.h
        HINTS "${NEMO_SPEECH_ROOT}/include")
    find_library(NEMO_SPEECH_ASR_LIBRARY
        NAMES nemo_speech_asr nemo-speech-asr
        HINTS "${NEMO_SPEECH_ROOT}/lib" "${NEMO_SPEECH_ROOT}/bin")
endif()

if(NEMO_SPEECH_INCLUDE_DIR AND NEMO_SPEECH_ASR_LIBRARY)
    target_compile_definitions(localflow_inference PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=1)
    target_include_directories(localflow_inference PRIVATE "${NEMO_SPEECH_INCLUDE_DIR}")
    target_link_libraries(localflow_inference PRIVATE "${NEMO_SPEECH_ASR_LIBRARY}")
else()
    target_compile_definitions(localflow_inference PRIVATE LOCALFLOW_HAVE_NEMO_SPEECH=0)
    message(STATUS "NeMo-Speech.cpp not found; ASR adapter will report unavailable")
endif()

if(LOCALFLOW_ENABLE_LLAMA)
    find_path(LLAMA_INCLUDE_DIR llama.h HINTS "${LLAMA_ROOT}/include")
    find_library(LLAMA_LIBRARY NAMES llama HINTS "${LLAMA_ROOT}/lib" "${LLAMA_ROOT}/bin")
endif()

if(LLAMA_INCLUDE_DIR AND LLAMA_LIBRARY)
    target_compile_definitions(localflow_inference PRIVATE LOCALFLOW_HAVE_LLAMA=1)
    target_include_directories(localflow_inference PRIVATE "${LLAMA_INCLUDE_DIR}")
    target_link_libraries(localflow_inference PRIVATE "${LLAMA_LIBRARY}")
else()
    target_compile_definitions(localflow_inference PRIVATE LOCALFLOW_HAVE_LLAMA=0)
    message(STATUS "llama.cpp not found; S1-mini adapter will report unavailable")
endif()

if(BUILD_TESTING)
    add_executable(localflow_inference_tests tests/InferenceTests.cpp)
    target_link_libraries(localflow_inference_tests PRIVATE LocalFlow::Inference)
    add_test(NAME localflow_inference_tests COMMAND localflow_inference_tests)
endif()
