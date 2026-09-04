#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

struct PlatformConfiguration {
    std::string hotkey{"RightCtrl"};
    std::string microphoneId;
    bool keepMicrophoneWarm{false};
    bool duckAudio{true};
    bool screenTerminology{true};
    int clipboardRestoreDelayMs{300};
};

enum class PlatformEventKind { began, level, ended, cancelled, error };

struct PlatformEvent {
    PlatformEventKind kind{PlatformEventKind::cancelled};
    std::uint64_t sessionId{0};
    float inputLevel{0};
    std::vector<float> samples;
    std::uint32_t sampleRate{16000};
    std::string targetAppId;
    std::shared_future<std::vector<std::string>> screenTerms;
    std::string message;
};

/// Native Windows/Linux services behind a small shared app-facing boundary.
/// Its callback may run on a native worker thread; the Qt controller must
/// marshal events to its own thread.
class PlatformBridge final {
public:
    using EventCallback = std::function<void(PlatformEvent)>;

    PlatformBridge();
    ~PlatformBridge();
    PlatformBridge(const PlatformBridge&) = delete;
    PlatformBridge& operator=(const PlatformBridge&) = delete;

    bool start(const PlatformConfiguration& configuration, EventCallback callback, std::string* error);
    void stop() noexcept;
    void setAcceptingInput(bool accepting) noexcept;
    bool insert(std::uint64_t sessionId, const std::string& text, std::string* error);
    void discardSession(std::uint64_t sessionId) noexcept;
    std::string capabilitySummary() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
