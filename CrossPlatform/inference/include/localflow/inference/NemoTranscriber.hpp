#pragma once

#include "localflow/inference/Inference.hpp"

#include <memory>
#include <string>

namespace localflow::inference {

class NemoTranscriber final : public ITranscriber {
public:
    struct Configuration {
        std::string modelPath;
        // -1 is the portable CPU baseline. Non-negative values select a GPU
        // exposed by the chosen NeMo-Speech.cpp backend build.
        int gpuDevice = -1;
        std::string languageCode;
    };

    explicit NemoTranscriber(Configuration configuration);
    ~NemoTranscriber() override;
    NemoTranscriber(const NemoTranscriber&) = delete;
    NemoTranscriber& operator=(const NemoTranscriber&) = delete;

    Result<bool> prepare() override;
    Result<Transcription> transcribe(const AudioBuffer& audio) override;

    static bool runtimeLinked() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace localflow::inference
