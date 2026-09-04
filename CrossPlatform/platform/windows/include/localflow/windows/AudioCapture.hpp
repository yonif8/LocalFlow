#pragma once

#ifdef _WIN32

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace localflow::windows {

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring display_name;
    bool is_default{false};
};

struct AudioChunk {
    /// Mono, normalized float PCM at the endpoint's native sample rate.
    std::vector<float> samples;
    std::uint32_t sample_rate_hz{0};
    std::chrono::steady_clock::time_point captured_at{};
    bool discontinuity{false};
};

struct AudioCaptureOptions {
    /// Null selects the current default communications capture endpoint.
    std::optional<std::wstring> endpoint_id;
    /// Keep the capture client running briefly after PTT release so frames
    /// already queued by the microphone driver are not truncated. Values are
    /// clamped to 250 ms; zero requests an immediate non-blocking drain.
    std::chrono::milliseconds stop_drain_timeout{100};
};

[[nodiscard]] std::vector<AudioDeviceInfo> enumerate_capture_devices(
    std::error_code& error);

/// Event-driven WASAPI shared-mode microphone capture. Format conversion and
/// channel downmix are local; resampling to Parakeet's 16 kHz input remains a
/// shared-core concern so every OS uses exactly the same resampler.
class WasapiMicrophoneCapture final {
public:
    using ChunkCallback = std::function<void(AudioChunk)>;
    using ErrorCallback = std::function<void(std::error_code)>;

    WasapiMicrophoneCapture(
        AudioCaptureOptions options, ChunkCallback on_chunk, ErrorCallback on_error = {});
    ~WasapiMicrophoneCapture();

    WasapiMicrophoneCapture(const WasapiMicrophoneCapture&) = delete;
    WasapiMicrophoneCapture& operator=(const WasapiMicrophoneCapture&) = delete;

    [[nodiscard]] std::error_code start();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept { return running_.load(); }

private:
    void capture_main(std::promise<std::error_code> ready) noexcept;

    AudioCaptureOptions options_;
    ChunkCallback on_chunk_;
    ErrorCallback on_error_;
    std::thread capture_thread_;
    HANDLE stop_event_{nullptr};
    std::atomic<bool> running_{false};
    std::mutex lifecycle_mutex_;
};

}  // namespace localflow::windows

#endif
