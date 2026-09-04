#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace localflow::inference {

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value), {}); }
    static Result failure(std::string error) { return Result(std::nullopt, std::move(error)); }

    explicit operator bool() const noexcept { return value_.has_value(); }
    const T& value() const { return value_.value(); }
    T& value() { return value_.value(); }
    T take() { return std::move(value_.value()); }
    const std::string& error() const noexcept { return error_; }

private:
    Result(std::optional<T> value, std::string error)
        : value_(std::move(value)), error_(std::move(error)) {}

    std::optional<T> value_;
    std::string error_;
};

struct AudioBuffer {
    std::vector<float> samples;
    std::int32_t sampleRate = 16'000;
};

struct Transcription {
    std::string text;
    std::chrono::milliseconds elapsed{};
};

enum class Tone {
    Casual,
    Neutral,
};

struct PolishRequest {
    std::string transcript;
    Tone tone = Tone::Neutral;
    std::size_t maxOutputTokens = 1'024;
    std::chrono::milliseconds timeout{1'500};
};

struct PolishResponse {
    std::string text;
    std::chrono::milliseconds elapsed{};
};

class ITranscriber {
public:
    virtual ~ITranscriber() = default;
    virtual Result<bool> prepare() = 0;
    virtual Result<Transcription> transcribe(const AudioBuffer& audio) = 0;
};

class IPolishModel {
public:
    virtual ~IPolishModel() = default;
    virtual Result<bool> prepare() = 0;
    virtual Result<PolishResponse> polish(const PolishRequest& request) = 0;
};

}  // namespace localflow::inference
