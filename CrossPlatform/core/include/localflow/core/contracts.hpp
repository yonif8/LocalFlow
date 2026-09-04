#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace localflow::core {

struct Utterance {
    std::vector<float> samples;
    double sample_rate_hz{16'000.0};

    [[nodiscard]] double duration_seconds() const noexcept {
        return sample_rate_hz > 0.0
            ? static_cast<double>(samples.size()) / sample_rate_hz
            : 0.0;
    }
};

enum class CaptureEventKind { began, ended, cancelled, level };

struct CaptureEvent {
    CaptureEventKind kind{CaptureEventKind::cancelled};
    std::optional<Utterance> utterance;
    float input_level{0.0F};

    static CaptureEvent began() { return {CaptureEventKind::began, std::nullopt, 0.0F}; }
    static CaptureEvent ended(Utterance value) {
        return {CaptureEventKind::ended, std::move(value), 0.0F};
    }
    static CaptureEvent cancelled() {
        return {CaptureEventKind::cancelled, std::nullopt, 0.0F};
    }
    static CaptureEvent level(float value) {
        return {CaptureEventKind::level, std::nullopt, value};
    }
};

struct PolishContext {
    // Bundle ID on macOS, Application User Model ID on Windows, or desktop ID
    // on Linux. Core policy treats it as an opaque stable application ID.
    std::optional<std::string> target_app_id;
};

class ITranscriber {
public:
    virtual ~ITranscriber() = default;
    virtual std::string transcribe(const Utterance& utterance) = 0;
};

class ICaptureEngine {
public:
    using EventHandler = std::function<void(const CaptureEvent&)>;

    virtual ~ICaptureEngine() = default;
    virtual void set_event_handler(EventHandler handler) = 0;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;
};

class ITextPolisher {
public:
    virtual ~ITextPolisher() = default;
    // Implementations should fail open internally when possible. The shared
    // pipeline also catches failures and restores its deterministic input.
    virtual std::string polish(
        const std::string& text,
        const PolishContext& context) = 0;
};

class ITextInserter {
public:
    virtual ~ITextInserter() = default;
    virtual void insert(const std::string& text) = 0;
};

}  // namespace localflow::core
