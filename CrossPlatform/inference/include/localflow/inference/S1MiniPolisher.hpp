#pragma once

#include "localflow/inference/Inference.hpp"

#include <memory>
#include <string>

namespace localflow::inference {

class S1MiniPolisher final : public IPolishModel {
public:
    struct Configuration {
        std::string modelPath;
        std::uint32_t contextTokens = 4'096;
        // 0 is CPU-only. A large value requests all supported layers on the
        // selected llama.cpp accelerator backend.
        int gpuLayers = 0;
    };

    explicit S1MiniPolisher(Configuration configuration);
    ~S1MiniPolisher() override;
    S1MiniPolisher(const S1MiniPolisher&) = delete;
    S1MiniPolisher& operator=(const S1MiniPolisher&) = delete;

    Result<bool> prepare() override;
    Result<PolishResponse> polish(const PolishRequest& request) override;

    static bool runtimeLinked() noexcept;
    static std::string promptFor(const PolishRequest& request);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace localflow::inference
