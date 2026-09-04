#include "localflow/linux/Audio.hpp"

#include "InternalFactories.hpp"
#include "Process.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace localflow::platform::linux {
namespace {

class UnavailableCapture final : public AudioCaptureBackend {
public:
    explicit UnavailableCapture(Status status) : status_(std::move(status)) {}
    std::string name() const override { return "unavailable"; }
    Result<std::vector<AudioDevice>> devices() override {
        return Result<std::vector<AudioDevice>>::failure(status_);
    }
    Status start(const std::string&, CaptureFormat, AudioSamplesCallback, AudioFailureCallback) override {
        return status_;
    }
    void stop() noexcept override {}
private:
    Status status_;
};

class UnavailableDucker final : public AudioDucker {
public:
    explicit UnavailableDucker(Status status) : status_(std::move(status)) {}
    Status duck(float) override { return status_; }
    void restore() noexcept override {}
private:
    Status status_;
};

Status capabilityFailure(const CapabilityReport& report, Feature feature) {
    const auto* capability = report.find(feature);
    return Status::failure(
        ErrorCode::missing_dependency,
        capability ? capability->detail : "Linux audio capability detection was not run.",
        capability ? capability->remediation : std::string{});
}

}  // namespace

std::unique_ptr<AudioCaptureBackend> makeAudioCaptureBackend(const CapabilityReport& report) {
    const auto* capability = report.find(Feature::microphone_capture);
    if (!capability || !capability->usable()) {
        return std::make_unique<UnavailableCapture>(capabilityFailure(
            report, Feature::microphone_capture));
    }
    return detail::makeProcessAudioCapture(capability->backend);
}

std::unique_ptr<AudioDucker> makeAudioDucker(const CapabilityReport& report) {
    const auto* capability = report.find(Feature::audio_ducking);
    if (!capability || !capability->usable()) {
        return std::make_unique<UnavailableDucker>(capabilityFailure(
            report, Feature::audio_ducking));
    }
    return detail::makeProcessAudioDucker(capability->backend);
}

}  // namespace localflow::platform::linux

namespace localflow::platform::linux::detail {
namespace {

class ProcessCapture final : public AudioCaptureBackend {
public:
    explicit ProcessCapture(std::string backend) : backend_(std::move(backend)) {}
    ~ProcessCapture() override { stop(); }

    std::string name() const override { return backend_; }

    Result<std::vector<AudioDevice>> devices() override {
        return Result<std::vector<AudioDevice>>::success({
            {"", "System default microphone", true},
        });
    }

    Status start(
        const std::string& deviceId,
        CaptureFormat format,
        AudioSamplesCallback samples,
        AudioFailureCallback failure) override {
        if (!samples || format.sampleRate < 8000 || format.sampleRate > 192000 ||
            format.channels == 0 || format.channels > 8) {
            return Status::failure(ErrorCode::invalid_argument, "Invalid microphone capture configuration.");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return Status::failure(ErrorCode::busy, "Microphone capture is already active.");
        }

        const auto arguments = makeArguments(deviceId, format);
        if (arguments.empty() || !executableOnPath(arguments.front())) {
            return Status::failure(
                ErrorCode::missing_dependency,
                backend_ + " was detected, but its recorder command is missing.",
                backend_.find("PipeWire") != std::string::npos
                    ? "Install pw-record (usually pipewire-bin)."
                    : "Install parec (usually pulseaudio-utils).");
        }

        int descriptors[2]{-1, -1};
        if (::pipe(descriptors) != 0) {
            return Status::failure(ErrorCode::io_error, "Could not create the microphone audio pipe.");
        }
        const pid_t child = ::fork();
        if (child == 0) {
            const int nullInput = ::open("/dev/null", O_RDONLY);
            const int nullError = ::open("/dev/null", O_WRONLY);
            if (nullInput >= 0) (void)::dup2(nullInput, STDIN_FILENO);
            (void)::dup2(descriptors[1], STDOUT_FILENO);
            if (nullError >= 0) (void)::dup2(nullError, STDERR_FILENO);
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            if (nullInput >= 0) ::close(nullInput);
            if (nullError >= 0) ::close(nullError);
            std::vector<char*> argv;
            for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            _exit(127);
        }
        ::close(descriptors[1]);
        if (child < 0) {
            ::close(descriptors[0]);
            return Status::failure(ErrorCode::io_error, "Could not launch the microphone recorder.");
        }

        child_ = child;
        descriptor_ = descriptors[0];
        samples_ = std::move(samples);
        failure_ = std::move(failure);
        stopping_ = false;
        running_ = true;
        reader_ = std::thread([this] { readAudio(); });
        return Status::success();
    }

    void stop() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (child_ <= 0) return;
            stopping_ = true;
            running_ = false;
            (void)::kill(child_, SIGTERM);
        }
        if (reader_.joinable()) reader_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        int status = 0;
        if (::waitpid(child_, &status, WNOHANG) == 0) {
            (void)::kill(child_, SIGKILL);
            (void)::waitpid(child_, &status, 0);
        }
        child_ = -1;
        stopping_ = false;
        samples_ = {};
        failure_ = {};
    }

private:
    std::vector<std::string> makeArguments(const std::string& device, CaptureFormat format) const {
        if (backend_.find("PipeWire") != std::string::npos) {
            std::vector<std::string> result{
                "pw-record", "--rate", std::to_string(format.sampleRate),
                "--channels", std::to_string(format.channels), "--format", "s16"};
            if (!device.empty()) result.insert(result.end(), {"--target", device});
            result.push_back("-");
            return result;
        }
        if (backend_.find("PulseAudio") != std::string::npos) {
            std::vector<std::string> result{
                "parec", "--raw", "--format=s16le",
                "--rate=" + std::to_string(format.sampleRate),
                "--channels=" + std::to_string(format.channels)};
            if (!device.empty()) result.push_back("--device=" + device);
            return result;
        }
        return {};
    }

    void readAudio() noexcept {
        std::array<std::uint8_t, 8192> bytes{};
        std::vector<float> floats(bytes.size() / 2);
        while (running_) {
            const auto count = ::read(descriptor_, bytes.data(), bytes.size());
            if (count > 0) {
                const auto countSamples = static_cast<std::size_t>(count) / 2;
                for (std::size_t index = 0; index < countSamples; ++index) {
                    const auto bits = static_cast<std::uint16_t>(bytes[index * 2]) |
                        (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8U);
                    floats[index] = static_cast<float>(static_cast<std::int16_t>(bits)) / 32768.0F;
                }
                try { samples_(floats.data(), countSamples); } catch (...) { break; }
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        bool intentional = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            intentional = stopping_;
            running_ = false;
        }
        if (!intentional && failure_) {
            try {
                failure_(Status::failure(
                    ErrorCode::io_error,
                    backend_ + " microphone capture stopped unexpectedly.",
                    "Check the microphone and desktop audio service."));
            } catch (...) {}
        }
    }

    std::string backend_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    bool stopping_{false};
    pid_t child_{-1};
    int descriptor_{-1};
    AudioSamplesCallback samples_;
    AudioFailureCallback failure_;
    std::thread reader_;
};

class ProcessDucker final : public AudioDucker {
public:
    explicit ProcessDucker(std::string backend) : backend_(std::move(backend)) {}
    ~ProcessDucker() override { restore(); }

    Status duck(float outputLevel) override {
        if (!std::isfinite(outputLevel) || outputLevel < 0.0F || outputLevel > 1.0F) {
            return Status::failure(ErrorCode::invalid_argument, "Ducking level must be between zero and one.");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (ducked_) return Status::success();
        const auto current = readVolume();
        if (!current) return current.status();
        const auto changed = setVolume(outputLevel);
        if (!changed.ok()) return changed;
        previous_ = current.value();
        ducked_ = true;
        return Status::success();
    }

    void restore() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ducked_) return;
        (void)setVolume(previous_);
        ducked_ = false;
    }

private:
    Result<float> readVolume() const {
        const bool wirePlumber = backend_.find("wpctl") != std::string::npos;
        const auto response = wirePlumber
            ? runCommand({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"})
            : runCommand({"pactl", "get-sink-volume", "@DEFAULT_SINK@"});
        if (!response.launched || response.timedOut || response.exitCode != 0) {
            return Result<float>::failure(Status::failure(ErrorCode::io_error, "Could not read output volume."));
        }
        try {
            if (wirePlumber) {
                const auto marker = response.standardOutput.find("Volume:");
                return Result<float>::success(std::stof(response.standardOutput.substr(marker + 7)));
            }
            const auto percent = response.standardOutput.find('%');
            const auto slash = response.standardOutput.rfind('/', percent);
            const auto digit = response.standardOutput.find_first_of("0123456789", slash);
            return Result<float>::success(std::stof(response.standardOutput.substr(digit, percent - digit)) / 100.0F);
        } catch (...) {
            return Result<float>::failure(Status::failure(ErrorCode::protocol_error, "Could not parse output volume."));
        }
    }

    Status setVolume(float value) const {
        const auto percent = std::to_string(static_cast<int>(std::lround(value * 100.0F))) + "%";
        const auto response = backend_.find("wpctl") != std::string::npos
            ? runCommand({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", percent})
            : runCommand({"pactl", "set-sink-volume", "@DEFAULT_SINK@", percent});
        return response.launched && !response.timedOut && response.exitCode == 0
            ? Status::success()
            : Status::failure(ErrorCode::io_error, "Could not change output volume.");
    }

    std::string backend_;
    std::mutex mutex_;
    float previous_{1.0F};
    bool ducked_{false};
};

}  // namespace

std::unique_ptr<AudioCaptureBackend> makeProcessAudioCapture(std::string backend) {
    return std::make_unique<ProcessCapture>(std::move(backend));
}

std::unique_ptr<AudioDucker> makeProcessAudioDucker(std::string backend) {
    return std::make_unique<ProcessDucker>(std::move(backend));
}

}  // namespace localflow::platform::linux::detail
