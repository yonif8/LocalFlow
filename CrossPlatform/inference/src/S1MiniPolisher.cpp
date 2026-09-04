#include "localflow/inference/S1MiniPolisher.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#if LOCALFLOW_HAVE_LLAMA
#include <llama.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#endif

namespace localflow::inference {
namespace {

constexpr const char* kSystemPrompt =
    "You are a text normalizer for speech-to-text transcripts. The input begins "
    "with a control line specifying the styling, structure, and context settings; "
    "clean the transcript to match those settings and output only the cleaned text.";

#if LOCALFLOW_HAVE_LLAMA
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

class LlamaRuntime {
public:
    LlamaRuntime() = default;
    LlamaRuntime(const LlamaRuntime&) = delete;
    LlamaRuntime& operator=(const LlamaRuntime&) = delete;

    ~LlamaRuntime() {
        if (backendInitialized_ && backendFree != nullptr) backendFree();
        closeModule();
    }

    std::string load() {
        if (module_ != nullptr) return {};
        for (const auto& candidate : libraryCandidates()) {
            if (!openModule(candidate)) continue;
            if (resolveSymbols()) return {};
            closeModule();
        }
        return "Could not load the bundled llama.cpp runtime. Reinstall LocalFlow to repair its polish engine.";
    }

    std::size_t initializeBackend() {
        if (!backendInitialized_) {
            const std::string backendDirectory = moduleDirectory_.u8string();
            backendLoadAllFromPath(backendDirectory.empty() ? nullptr : backendDirectory.c_str());
            backendInit();
            backendInitialized_ = true;
        }
        return backendDeviceCount();
    }

    using BackendLoadAllFromPath = void (*)(const char*);
    using BackendDeviceCount = std::size_t (*)();
    BackendLoadAllFromPath backendLoadAllFromPath = nullptr;
    BackendDeviceCount backendDeviceCount = nullptr;
    decltype(&::llama_backend_init) backendInit = nullptr;
    decltype(&::llama_backend_free) backendFree = nullptr;
    decltype(&::llama_model_default_params) modelDefaultParams = nullptr;
    decltype(&::llama_model_load_from_file) modelLoadFromFile = nullptr;
    decltype(&::llama_model_free) modelFree = nullptr;
    decltype(&::llama_context_default_params) contextDefaultParams = nullptr;
    decltype(&::llama_init_from_model) initFromModel = nullptr;
    decltype(&::llama_free) freeContext = nullptr;
    decltype(&::llama_set_abort_callback) setAbortCallback = nullptr;
    decltype(&::llama_model_get_vocab) modelGetVocab = nullptr;
    decltype(&::llama_tokenize) tokenize = nullptr;
    decltype(&::llama_sampler_init_greedy) samplerInitGreedy = nullptr;
    decltype(&::llama_sampler_free) samplerFree = nullptr;
    decltype(&::llama_batch_get_one) batchGetOne = nullptr;
    decltype(&::llama_decode) decode = nullptr;
    decltype(&::llama_sampler_sample) samplerSample = nullptr;
    decltype(&::llama_vocab_is_eog) vocabIsEog = nullptr;
    decltype(&::llama_token_to_piece) tokenToPiece = nullptr;

private:
#ifdef _WIN32
    HMODULE module_ = nullptr;
    // llama.dll imports ggml.dll, but Windows does not expose dependency
    // exports through GetProcAddress(). Keep an explicit reference so the two
    // ggml backend entry points can be resolved from the DLL that owns them.
    HMODULE ggmlModule_ = nullptr;
#else
    void* module_ = nullptr;
#endif
    std::filesystem::path moduleDirectory_;
    bool backendInitialized_ = false;

    static std::filesystem::path executableDirectory() {
#ifdef _WIN32
        std::wstring buffer(32768, L'\0');
        const DWORD size = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0 || size >= static_cast<DWORD>(buffer.size())) return {};
        buffer.resize(size);
        return std::filesystem::path(buffer).parent_path();
#else
        std::vector<char> buffer(4096, '\0');
        const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (size <= 0) return {};
        return std::filesystem::path(
            std::string(buffer.data(), static_cast<std::size_t>(size))).parent_path();
#endif
    }

    static std::vector<std::filesystem::path> libraryCandidates() {
        std::vector<std::filesystem::path> result;
#ifdef _WIN32
        const DWORD required = GetEnvironmentVariableW(L"LOCALFLOW_LLAMA_LIBRARY", nullptr, 0);
        if (required > 1) {
            std::vector<wchar_t> configured(required, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                L"LOCALFLOW_LLAMA_LIBRARY", configured.data(), required);
            if (copied > 0 && copied < required) {
                std::error_code error;
                auto configuredPath = std::filesystem::path(configured.data());
                if (configuredPath.is_relative()) {
                    configuredPath = std::filesystem::absolute(configuredPath, error);
                }
                if (!error) result.emplace_back(std::move(configuredPath));
            }
        }
#else
        if (const char* configured = std::getenv("LOCALFLOW_LLAMA_LIBRARY");
            configured != nullptr && *configured != '\0') {
            std::error_code error;
            auto configuredPath = std::filesystem::u8path(configured);
            if (configuredPath.is_relative()) {
                configuredPath = std::filesystem::absolute(configuredPath, error);
            }
            if (!error) result.emplace_back(std::move(configuredPath));
        }
#endif
        const auto executable = executableDirectory();
        if (!executable.empty()) {
#ifdef _WIN32
            result.emplace_back(executable / L"llama.dll");
#else
            result.emplace_back(executable / "libllama.so");
#endif
        }
#ifdef _WIN32
        result.emplace_back(L"llama.dll");
#else
        result.emplace_back("libllama.so");
#endif
        return result;
    }

    bool openModule(const std::filesystem::path& candidate) {
#ifdef _WIN32
        const DWORD flags = candidate.is_absolute()
            ? LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
            : LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        const std::filesystem::path ggmlCandidate = candidate.has_parent_path()
            ? candidate.parent_path() / L"ggml.dll"
            : std::filesystem::path(L"ggml.dll");
        ggmlModule_ = LoadLibraryExW(ggmlCandidate.c_str(), nullptr, flags);
        if (ggmlModule_ == nullptr) return false;
        module_ = LoadLibraryExW(candidate.c_str(), nullptr, flags);
        if (module_ == nullptr) {
            FreeLibrary(ggmlModule_);
            ggmlModule_ = nullptr;
            return false;
        }
#else
        module_ = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (module_ != nullptr) {
            std::error_code error;
            moduleDirectory_ = candidate.has_parent_path()
                ? candidate.parent_path()
                : std::filesystem::current_path(error);
            if (error) moduleDirectory_.clear();
        }
        return module_ != nullptr;
    }

    void closeModule() {
#ifdef _WIN32
        // Release llama first: it imports ggml, and our explicit ggml handle
        // keeps that dependency alive through llama's detach handlers.
        if (module_ != nullptr) FreeLibrary(module_);
        module_ = nullptr;
        if (ggmlModule_ != nullptr) FreeLibrary(ggmlModule_);
        ggmlModule_ = nullptr;
#else
        if (module_ != nullptr) ::dlclose(module_);
        module_ = nullptr;
#endif
        moduleDirectory_.clear();
    }

    template <typename Function>
    bool resolveFrom(
#ifdef _WIN32
        HMODULE source,
#else
        void* source,
#endif
        Function& destination, const char* name) {
#ifdef _WIN32
        const auto raw = GetProcAddress(source, name);
#else
        const auto raw = ::dlsym(source, name);
#endif
        if (raw == nullptr) return false;
        static_assert(sizeof(destination) == sizeof(raw),
            "Dynamic function pointers must fit the platform symbol type");
        std::memcpy(&destination, &raw, sizeof(destination));
        return true;
    }

    bool resolveSymbols() {
#ifdef _WIN32
        const auto backendModule = ggmlModule_;
#else
        const auto backendModule = module_;
#endif
        return resolveFrom(
                   backendModule, backendLoadAllFromPath,
                   "ggml_backend_load_all_from_path")
            && resolveFrom(
                   backendModule, backendDeviceCount,
                   "ggml_backend_dev_count")
            && resolveFrom(module_, backendInit, "llama_backend_init")
            && resolveFrom(module_, backendFree, "llama_backend_free")
            && resolveFrom(module_, modelDefaultParams, "llama_model_default_params")
            && resolveFrom(module_, modelLoadFromFile, "llama_model_load_from_file")
            && resolveFrom(module_, modelFree, "llama_model_free")
            && resolveFrom(module_, contextDefaultParams, "llama_context_default_params")
            && resolveFrom(module_, initFromModel, "llama_init_from_model")
            && resolveFrom(module_, freeContext, "llama_free")
            && resolveFrom(module_, setAbortCallback, "llama_set_abort_callback")
            && resolveFrom(module_, modelGetVocab, "llama_model_get_vocab")
            && resolveFrom(module_, tokenize, "llama_tokenize")
            && resolveFrom(module_, samplerInitGreedy, "llama_sampler_init_greedy")
            && resolveFrom(module_, samplerFree, "llama_sampler_free")
            && resolveFrom(module_, batchGetOne, "llama_batch_get_one")
            && resolveFrom(module_, decode, "llama_decode")
            && resolveFrom(module_, samplerSample, "llama_sampler_sample")
            && resolveFrom(module_, vocabIsEog, "llama_vocab_is_eog")
            && resolveFrom(module_, tokenToPiece, "llama_token_to_piece");
    }
};
#endif

}  // namespace

struct S1MiniPolisher::Implementation {
    explicit Implementation(Configuration value) : configuration(std::move(value)) {}
    Configuration configuration;
    std::mutex mutex;
#if LOCALFLOW_HAVE_LLAMA
    std::unique_ptr<LlamaRuntime> runtime;
    llama_model* model = nullptr;
#endif
};

S1MiniPolisher::S1MiniPolisher(Configuration configuration)
    : implementation_(std::make_unique<Implementation>(std::move(configuration))) {}

S1MiniPolisher::~S1MiniPolisher() {
#if LOCALFLOW_HAVE_LLAMA
    std::lock_guard lock(implementation_->mutex);
    if (implementation_->model != nullptr) {
        implementation_->runtime->modelFree(implementation_->model);
        implementation_->model = nullptr;
    }
    implementation_->runtime.reset();
#endif
}

bool S1MiniPolisher::runtimeLinked() noexcept {
#if LOCALFLOW_HAVE_LLAMA
    return true;
#else
    return false;
#endif
}

Result<bool> S1MiniPolisher::probeRuntime() {
#if !LOCALFLOW_HAVE_LLAMA
    return Result<bool>::failure("llama.cpp headers were not available when LocalFlow was built");
#else
    auto runtime = std::make_unique<LlamaRuntime>();
    if (const auto loadError = runtime->load(); !loadError.empty()) {
        return Result<bool>::failure(loadError);
    }
    if (runtime->initializeBackend() == 0) {
        return Result<bool>::failure(
            "The bundled llama.cpp runtime did not load a compute backend. Reinstall LocalFlow to repair its polish engine.");
    }
    return Result<bool>::success(true);
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
    std::error_code modelError;
    if (!std::filesystem::is_regular_file(
            std::filesystem::u8path(implementation_->configuration.modelPath), modelError)) {
        return Result<bool>::failure("S1-mini model is missing: " + implementation_->configuration.modelPath);
    }

    auto runtime = std::make_unique<LlamaRuntime>();
    if (const auto loadError = runtime->load(); !loadError.empty()) {
        return Result<bool>::failure(loadError);
    }
    if (runtime->initializeBackend() == 0) {
        return Result<bool>::failure(
            "The bundled llama.cpp runtime did not load a compute backend. Reinstall LocalFlow to repair its polish engine.");
    }
    auto parameters = runtime->modelDefaultParams();
    parameters.n_gpu_layers = implementation_->configuration.gpuLayers;
    implementation_->model = runtime->modelLoadFromFile(
        implementation_->configuration.modelPath.c_str(), parameters);
    if (implementation_->model == nullptr) {
        return Result<bool>::failure("Could not load S1-mini with llama.cpp");
    }
    implementation_->runtime = std::move(runtime);
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

    auto* const runtime = implementation_->runtime.get();
    auto contextParameters = runtime->contextDefaultParams();
    contextParameters.n_ctx = implementation_->configuration.contextTokens;
    contextParameters.n_batch = implementation_->configuration.contextTokens;
    llama_context* context = runtime->initFromModel(implementation_->model, contextParameters);
    if (context == nullptr) {
        return Result<PolishResponse>::failure("Could not create the S1-mini context");
    }
    struct ContextGuard {
        decltype(&::llama_free) freeFunction;
        llama_context* value;
        ~ContextGuard() { freeFunction(value); }
    } contextGuard{runtime->freeContext, context};

    struct DeadlineState {
        std::chrono::steady_clock::time_point deadline;
    } deadlineState{deadline};
    runtime->setAbortCallback(context, [](void* data) {
        const auto* state = static_cast<const DeadlineState*>(data);
        return std::chrono::steady_clock::now() >= state->deadline;
    }, &deadlineState);

    const auto prompt = promptFor(request);
    const llama_vocab* vocabulary = runtime->modelGetVocab(implementation_->model);
    const int tokenCount = -runtime->tokenize(
        vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    if (tokenCount <= 0 || static_cast<std::uint32_t>(tokenCount) >= contextParameters.n_ctx) {
        return Result<PolishResponse>::failure("S1-mini input exceeds its context window");
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(tokenCount));
    if (runtime->tokenize(vocabulary, prompt.data(), static_cast<int32_t>(prompt.size()),
                          tokens.data(), tokenCount, true, true) < 0) {
        return Result<PolishResponse>::failure("Could not tokenize the S1-mini request");
    }

    llama_sampler* sampler = runtime->samplerInitGreedy();
    if (sampler == nullptr) {
        return Result<PolishResponse>::failure("Could not create the S1-mini sampler");
    }
    struct SamplerGuard {
        decltype(&::llama_sampler_free) freeFunction;
        llama_sampler* value;
        ~SamplerGuard() { freeFunction(value); }
    } samplerGuard{runtime->samplerFree, sampler};

    auto batch = runtime->batchGetOne(tokens.data(), static_cast<int32_t>(tokens.size()));
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
        const int decodeStatus = runtime->decode(context, batch);
        if (decodeStatus != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return Result<PolishResponse>::failure("S1-mini polish timed out");
            }
            return Result<PolishResponse>::failure("S1-mini generation failed");
        }

        sampledToken = runtime->samplerSample(sampler, context, -1);
        if (runtime->vocabIsEog(vocabulary, sampledToken)) {
            reachedEnd = true;
            break;
        }

        std::vector<char> piece(256);
        int pieceSize = runtime->tokenToPiece(
            vocabulary, sampledToken, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
        if (pieceSize < 0) {
            piece.resize(static_cast<std::size_t>(-pieceSize));
            pieceSize = runtime->tokenToPiece(
                vocabulary, sampledToken, piece.data(), static_cast<int32_t>(piece.size()), 0, true);
        }
        if (pieceSize < 0) {
            return Result<PolishResponse>::failure("Could not decode S1-mini output");
        }
        output.append(piece.data(), static_cast<std::size_t>(pieceSize));
        batch = runtime->batchGetOne(&sampledToken, 1);
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
