#include "localflow/inference/S1MiniPolisher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#if LOCALFLOW_HAVE_LLAMA
#include <llama.h>
#endif

namespace localflow::inference {
namespace {

constexpr const char* kSystemPrompt =
    "You are a text normalizer for speech-to-text transcripts. The input begins "
    "with a control line specifying the styling, structure, and context settings; "
    "clean the transcript to match those settings and output only the cleaned text.";

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::set<std::string> content_words(const std::string& text) {
    static const std::unordered_set<std::string> numberWords{
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
        "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
        "hundred", "thousand", "million", "billion", "point", "half", "quarter",
        "dollar", "dollars", "cents", "euros", "pounds", "percent", "oclock",
    };
    std::set<std::string> result;
    std::string current;
    bool hasDigit = false;
    const auto flush = [&] {
        if (current.size() > 2 && !hasDigit && numberWords.count(current) == 0) {
            result.insert(current);
        }
        current.clear();
        hasDigit = false;
    };
    for (const unsigned char byte : text) {
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9') || byte >= 0x80U) {
            current.push_back(byte >= 'A' && byte <= 'Z' ? char(byte + 32U) : char(byte));
            hasDigit = hasDigit || (byte >= '0' && byte <= '9');
        } else {
            flush();
        }
    }
    flush();
    return result;
}

bool looks_like_cleanup(const std::string& input, const std::string& candidate) {
    if (candidate.size() * 3 < input.size() || candidate.size() > input.size() * 2) {
        return false;
    }
    const auto source = content_words(input);
    const auto output = content_words(candidate);
    if (source.empty() || output.empty()) return true;
    std::size_t overlap = 0;
    for (const auto& word : output) overlap += source.count(word);
    return double(overlap) >= 0.6 * double(output.size());
}

}  // namespace

struct S1MiniPolisher::Implementation {
    explicit Implementation(Configuration value) : configuration(std::move(value)) {}
    Configuration configuration;
    std::mutex mutex;
#if LOCALFLOW_HAVE_LLAMA
    llama_model* model = nullptr;
#endif
};

S1MiniPolisher::S1MiniPolisher(Configuration configuration)
    : implementation_(std::make_unique<Implementation>(std::move(configuration))) {}

S1MiniPolisher::~S1MiniPolisher() {
#if LOCALFLOW_HAVE_LLAMA
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->model != nullptr) {
        llama_model_free(implementation_->model);
        implementation_->model = nullptr;
    }
#endif
}

bool S1MiniPolisher::runtimeLinked() noexcept {
#if LOCALFLOW_HAVE_LLAMA
    return true;
#else
    return false;
#endif
}

std::string S1MiniPolisher::promptFor(const PolishRequest& request) {
    // S1-mini was trained with Qwen3 thinking disabled. Construct the documented
    // prefix directly so this remains correct even when a runtime cannot pass
    // `enable_thinking=false` into a Jinja chat template.
    const char* styling = "semi-formal";
    return std::string("<|im_start|>system\n") + kSystemPrompt +
        "<|im_end|>\n<|im_start|>user\n[Styling: " + styling +
        "] [Structure: prose] [Context: general]\n" + request.transcript +
        "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
}

Result<bool> S1MiniPolisher::prepare() {
#if !LOCALFLOW_HAVE_LLAMA
    return Result<bool>::failure("llama.cpp was not linked into this build");
#else
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->model != nullptr) return Result<bool>::success(true);
    if (implementation_->configuration.modelPath.empty()) {
        return Result<bool>::failure("S1-mini model path is empty");
    }
    if (!std::filesystem::is_regular_file(implementation_->configuration.modelPath)) {
        return Result<bool>::failure("S1-mini model is missing: " + implementation_->configuration.modelPath);
    }

    llama_backend_init();
    auto parameters = llama_model_default_params();
    parameters.n_gpu_layers = implementation_->configuration.gpuLayers;
    implementation_->model = llama_model_load_from_file(
        implementation_->configuration.modelPath.c_str(), parameters);
    if (implementation_->model == nullptr) {
        return Result<bool>::failure("Could not load S1-mini with llama.cpp");
    }
    return Result<bool>::success(true);
#endif
}

Result<PolishResponse> S1MiniPolisher::polish(const PolishRequest& request) {
    if (request.transcript.empty()) {
        return Result<PolishResponse>::success({{}, std::chrono::milliseconds(0)});
    }
    const auto prepared = prepare();
    if (!prepared) return Result<PolishResponse>::failure(prepared.error());

#if !LOCALFLOW_HAVE_LLAMA
    return Result<PolishResponse>::failure("llama.cpp was not linked into this build");
#else
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + request.timeout;
    std::lock_guard lock(implementation_->mutex);

    auto contextParameters = llama_context_default_params();
    contextParameters.n_ctx = implementation_->configuration.contextTokens;
    contextParameters.n_batch = implementation_->configuration.contextTokens;
    llama_context* context = llama_init_from_model(implementation_->model, contextParameters);
    if (context == nullptr) {
        return Result<PolishResponse>::failure("Could not create the S1-mini context");
    }
    struct ContextGuard {
        llama_context* value;
        ~ContextGuard() { llama_free(value); }
    } contextGuard{context};

    struct DeadlineState {
        std::chrono::steady_clock::time_point deadline;
    } deadlineState{deadline};
    llama_set_abort_callback(context, [](void* data) {
        const auto* state = static_cast<const DeadlineState*>(data);
        return std::chrono::steady_clock::now() >= state->deadline;
    }, &deadlineState);

    const auto prompt = promptFor(request);
    const llama_vocab* vocabulary = llama_model_get_vocab(implementation_->model);
    const int tokenCount = -llama_tokenize(
        vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    if (tokenCount <= 0 || static_cast<std::uint32_t>(tokenCount) >= contextParameters.n_ctx) {
        return Result<PolishResponse>::failure("S1-mini input exceeds its context window");
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(tokenCount));
    if (llama_tokenize(vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()),
                       tokens.data(), tokenCount, true, true) < 0) {
        return Result<PolishResponse>::failure("Could not tokenize the S1-mini request");
    }

    llama_sampler* sampler = llama_sampler_init_greedy();
    if (sampler == nullptr) {
        return Result<PolishResponse>::failure("Could not create the S1-mini sampler");
    }
    struct SamplerGuard {
        llama_sampler* value;
        ~SamplerGuard() { llama_sampler_free(value); }
    } samplerGuard{sampler};

    auto batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()));
    std::string output;
    llama_token sampledToken = 0;
    bool reachedEnd = false;
    const std::size_t outputLimit = std::min<std::size_t>(
        request.maxOutputTokens,
        std::size_t(contextParameters.n_ctx) - std::size_t(tokenCount) - 1);
    for (std::size_t generated = 0; generated < outputLimit; ++generated) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return Result<PolishResponse>::failure("S1-mini polish timed out");
        }
        const int decodeStatus = llama_decode(context, batch);
        if (decodeStatus != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<PolishResponse>::failure("S1-mini polish timed out");
            }
            return Result<PolishResponse>::failure("S1-mini generation failed");
        }

        sampledToken = llama_sampler_sample(sampler, context, -1);
        if (llama_vocab_is_eog(vocabulary, sampledToken)) {
            reachedEnd = true;
            break;
        }

        std::vector<char> piece(256);
        int pieceSize = llama_token_to_piece(
            vocabulary, sampledToken, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
        if (pieceSize < 0) {
            piece.resize(static_cast<std::size_t>(-pieceSize));
            pieceSize = llama_token_to_piece(
                vocabulary, sampledToken, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
        }
        if (pieceSize < 0) {
            return Result<PolishResponse>::failure("Could not decode S1-mini output");
        }
        output.append(piece.data(), static_cast<std::size_t>(pieceSize));
        batch = llama_batch_get_one(&sampledToken, 1);
    }

    if (!reachedEnd) {
        return Result<PolishResponse>::failure(
            "S1-mini output reached its generation limit before completion");
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    output = trim(std::move(output));
    if (const auto thinking = output.find("</think>"); thinking != std::string::npos) {
        output = trim(output.substr(thinking + std::string("</think>").size()));
    }
    if (output.empty()) {
        return Result<PolishResponse>::failure("S1-mini returned empty output");
    }
    if (!looks_like_cleanup(request.transcript, output)) {
        return Result<PolishResponse>::failure("S1-mini output was not a plausible transcript cleanup");
    }
    return Result<PolishResponse>::success({std::move(output), elapsed});
#endif
}

}  // namespace localflow::inference
