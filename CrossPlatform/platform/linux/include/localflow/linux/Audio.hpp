#pragma once

#include "localflow/linux/Capabilities.hpp"
#include "localflow/linux/Status.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace localflow::platform::linux {

struct AudioDevice {
    std::string id;
    std::string name;
    bool isDefault{false};
};

struct CaptureFormat {
    std::uint32_t sampleRate{16000};
    std::uint16_t channels{1};
};

using AudioSamplesCallback = std::function<void(const float*, std::size_t)>;
using AudioFailureCallback = std::function<void(const Status&)>;

class AudioCaptureBackend {
public:
    virtual ~AudioCaptureBackend() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual Result<std::vector<AudioDevice>> devices() = 0;
    virtual Status start(
        const std::string& deviceId,
        CaptureFormat format,
        AudioSamplesCallback samples,
        AudioFailureCallback failure) = 0;
    // Synchronous completion boundary. A backend may deliver its final queued
    // samples from the calling thread's stop interval; callers must keep the
    // active accumulator valid until this returns.
    virtual void stop() noexcept = 0;
};

class AudioDucker {
public:
    virtual ~AudioDucker() = default;

    virtual Status duck(float outputLevel) = 0;
    virtual void restore() noexcept = 0;
};

// The factory prefers PipeWire and falls back to PulseAudio. A backend may be
// provided by the app's media layer; the Linux boundary does not assume Qt.
[[nodiscard]] std::unique_ptr<AudioCaptureBackend> makeAudioCaptureBackend(
    const CapabilityReport& report);

[[nodiscard]] std::unique_ptr<AudioDucker> makeAudioDucker(
    const CapabilityReport& report);

}  // namespace localflow::platform::linux
