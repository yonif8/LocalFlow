#include "localflow/inference/NemoTranscriber.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <utility>

#if LOCALFLOW_HAVE_NEMO_SPEECH
#include <nemo_speech/asr.h>
#endif

namespace localflow::inference {

struct NemoTranscriber::Implementation {
    explicit Implementation(Configuration value) : configuration(std::move(value)) {}
    Configuration configuration;
    std::mutex mutex;
#if LOCALFLOW_HAVE_NEMO_SPEECH
    nemo_speech_asr_recognizer* recognizer = nullptr;
#endif
};

NemoTranscriber::NemoTranscriber(Configuration configuration)
    : implementation_(std::make_unique<Implementation>(std::move(configuration))) {}

NemoTranscriber::~NemoTranscriber() {
#if LOCALFLOW_HAVE_NEMO_SPEECH
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->recognizer != nullptr) {
        nemo_speech_asr_destroy(implementation_->recognizer);
        implementation_->recognizer = nullptr;
    }
#endif
}

bool NemoTranscriber::runtimeLinked() noexcept {
#if LOCALFLOW_HAVE_NEMO_SPEECH
    return true;
#else
    return false;
#endif
}

Result<bool> NemoTranscriber::prepare() {
#if !LOCALFLOW_HAVE_NEMO_SPEECH
    return Result<bool>::failure("NeMo-Speech.cpp was not linked into this build");
#else
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->recognizer != nullptr) {
        return Result<bool>::success(true);
    }
    if (implementation_->configuration.modelPath.empty()) {
        return Result<bool>::failure("Parakeet model path is empty");
    }
    if (!std::filesystem::is_regular_file(implementation_->configuration.modelPath)) {
        return Result<bool>::failure("Parakeet model is missing: " + implementation_->configuration.modelPath);
    }

    nemo_speech_asr_backend_config backend{};
    backend.size = sizeof(backend);
    backend.gpu = implementation_->configuration.gpuDevice;

    nemo_speech_asr_model_config model{};
    model.size = sizeof(model);
    model.path = implementation_->configuration.modelPath.c_str();

    nemo_speech_asr_recognizer_config configuration{};
    configuration.size = sizeof(configuration);
    configuration.backend = &backend;
    configuration.model = &model;

    const auto status = nemo_speech_asr_create(&configuration, &implementation_->recognizer);
    if (status != NEMO_SPEECH_ASR_OK || implementation_->recognizer == nullptr) {
        return Result<bool>::failure(std::string("Could not load Parakeet: ") +
                                     nemo_speech_asr_last_error());
    }
    return Result<bool>::success(true);
#endif
}

Result<Transcription> NemoTranscriber::transcribe(const AudioBuffer& audio) {
    if (audio.samples.size() <= 256) {
        return Result<Transcription>::failure("Utterance is too short to transcribe");
    }
    if (audio.sampleRate < 8'000 || audio.sampleRate > 96'000) {
        return Result<Transcription>::failure("Audio sample rate must be between 8 kHz and 96 kHz");
    }
    const auto prepared = prepare();
    if (!prepared) {
        return Result<Transcription>::failure(prepared.error());
    }

#if !LOCALFLOW_HAVE_NEMO_SPEECH
    return Result<Transcription>::failure("NeMo-Speech.cpp was not linked into this build");
#else
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard lock(implementation_->mutex);

    auto options = nemo_speech_asr_recognition_options_default();
    options.enable_automatic_punctuation = true;
    options.verbatim_transcripts = true;
    options.language_code = implementation_->configuration.languageCode.empty()
        ? nullptr : implementation_->configuration.languageCode.c_str();

    nemo_speech_asr_result* result = nullptr;
    const auto status = nemo_speech_asr_recognize_f32(
        implementation_->recognizer, &options, audio.samples.data(), audio.samples.size(),
        audio.sampleRate, &result);
    if (status != NEMO_SPEECH_ASR_OK || result == nullptr) {
        return Result<Transcription>::failure(std::string("Parakeet transcription failed: ") +
                                              nemo_speech_asr_last_error());
    }

    std::string text;
    if (nemo_speech_asr_result_alternative_count(result) > 0) {
        if (const char* value = nemo_speech_asr_result_transcript(result, 0)) {
            text = value;
        }
    }
    nemo_speech_asr_result_destroy(result);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return Result<Transcription>::success({std::move(text), elapsed});
#endif
}

}  // namespace localflow::inference
