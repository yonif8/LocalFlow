#include "localflow/linux/Audio.hpp"

#include "AudioSupport.hpp"
#include "InternalFactories.hpp"
#include "Process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
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
    explicit ProcessCapture(
        std::string backend,
        std::vector<std::string> fixedArguments = {})
        : backend_(std::move(backend)),
          fixedArguments_(std::move(fixedArguments)) {}
    ~ProcessCapture() override { stop(); }

    std::string name() const override { return backend_; }

    Result<std::vector<AudioDevice>> devices() override {
        // PipeWire normally exposes its graph through the PulseAudio
        // compatibility service. pactl source names are stable across boots
        // and accepted by both parec and pw-record's --target option.
        if (executableOnPath("pactl") && executableOnPath("env")) {
            const auto listed = runCommand(
                {"env", "LC_ALL=C", "pactl", "list", "sources"},
                {}, std::chrono::milliseconds(2000));
            const auto defaultSource = runCommand(
                {"pactl", "get-default-source"},
                {}, std::chrono::milliseconds(1000));
            if (listed.launched && !listed.timedOut && listed.exitCode == 0) {
                auto parsed = parsePactlSources(
                    listed.standardOutput,
                    defaultSource.launched && !defaultSource.timedOut &&
                            defaultSource.exitCode == 0
                        ? defaultSource.standardOutput
                        : std::string{});
                if (!parsed.empty()) {
                    return Result<std::vector<AudioDevice>>::success(std::move(parsed));
                }
                return Result<std::vector<AudioDevice>>::failure(Status::failure(
                    ErrorCode::service_unavailable,
                    "The audio service exposes only monitor outputs, not a physical microphone.",
                    "Connect or enable a microphone in the desktop sound settings."));
            }
        }
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
        if (running_ || child_ > 0 || reader_.joinable()) {
            return Status::failure(ErrorCode::busy, "Microphone capture is already active.");
        }

        std::string captureDevice = deviceId;
        if (isMonitorSource(captureDevice)) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "Monitor outputs cannot be selected as a dictation microphone.",
                "Choose a physical microphone in LocalFlow settings.");
        }
        if (captureDevice.empty() && executableOnPath("pactl")) {
            const auto defaultSource = runCommand(
                {"pactl", "get-default-source"},
                {}, std::chrono::milliseconds(750));
            if (defaultSource.launched && !defaultSource.timedOut &&
                defaultSource.exitCode == 0) {
                const auto parsedDefault = parseAudioServiceIdentifier(
                    defaultSource.standardOutput);
                if (parsedDefault && !isMonitorSource(parsedDefault.value())) {
                    captureDevice = parsedDefault.value();
                } else if (parsedDefault) {
                    const auto available = devices();
                    if (!available) return available.status();
                    const auto preferred = std::find_if(
                        available.value().begin(), available.value().end(),
                        [](const AudioDevice& device) { return device.isDefault; });
                    const auto selected = preferred != available.value().end()
                        ? preferred
                        : available.value().begin();
                    if (selected == available.value().end()) {
                        return Status::failure(
                            ErrorCode::service_unavailable,
                            "No physical microphone is available.");
                    }
                    captureDevice = selected->id;
                }
            }
        }

        const auto arguments = makeArguments(captureDevice, format);
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
        const int descriptorFlags = ::fcntl(descriptor_, F_GETFL, 0);
        if (descriptorFlags < 0 ||
            ::fcntl(descriptor_, F_SETFL, descriptorFlags | O_NONBLOCK) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            (void)::kill(child_, SIGKILL);
            int childStatus = 0;
            (void)::waitpid(child_, &childStatus, 0);
            child_ = -1;
            return Status::failure(
                ErrorCode::io_error,
                "Could not configure bounded microphone pipe reads.");
        }
        samples_ = std::move(samples);
        failure_ = std::move(failure);
        stopRequested_ = false;
        drainDeadlineNs_ = 0;
        running_ = true;
        reader_ = std::thread([this] { readAudio(); });
        return Status::success();
    }

    void stop() noexcept override {
        std::lock_guard<std::mutex> stopLock(stopMutex_);
        pid_t child = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (child_ <= 0 && !reader_.joinable()) return;
            drainDeadlineNs_ = steadyNowNs() +
                std::chrono::duration_cast<std::chrono::nanoseconds>(kDrainTimeout).count();
            // Publish the initialized deadline before publishing the stop.
            stopRequested_ = true;
            child = child_;
        }

        // SIGINT lets pw-record/parec finalize their current capture buffer.
        // The reader remains alive until EOF or the bounded drain deadline.
        if (child > 0) (void)::kill(child, SIGINT);
        if (reader_.joinable()) reader_.join();

        std::lock_guard<std::mutex> lock(mutex_);
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        int status = 0;
        bool exited = child_ <= 0;
        for (int attempt = 0; !exited && attempt < 10; ++attempt) {
            const auto waited = ::waitpid(child_, &status, WNOHANG);
            exited = waited == child_ || (waited < 0 && errno == ECHILD);
            if (!exited) ::usleep(10'000);
        }
        if (!exited) {
            (void)::kill(child_, SIGTERM);
            for (int attempt = 0; !exited && attempt < 10; ++attempt) {
                const auto waited = ::waitpid(child_, &status, WNOHANG);
                exited = waited == child_ || (waited < 0 && errno == ECHILD);
                if (!exited) ::usleep(10'000);
            }
        }
        if (!exited) {
            (void)::kill(child_, SIGKILL);
            (void)::waitpid(child_, &status, 0);
        }
        child_ = -1;
        running_ = false;
        stopRequested_ = false;
        drainDeadlineNs_ = 0;
        samples_ = {};
        failure_ = {};
    }

private:
    static constexpr auto kDrainTimeout = std::chrono::milliseconds(500);

    static bool isMonitorSource(const std::string& source) noexcept {
        constexpr const char suffix[] = ".monitor";
        return source.size() >= sizeof(suffix) - 1 &&
            source.compare(source.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0;
    }

    static std::int64_t steadyNowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::vector<std::string> makeArguments(const std::string& device, CaptureFormat format) const {
        if (!fixedArguments_.empty()) return fixedArguments_;
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
        std::vector<float> floats;
        S16LePcmDecoder decoder;
        bool reachedEof = false;
        bool readFailed = false;
        bool callbackFailed = false;

        while (!reachedEof && !readFailed && !callbackFailed) {
            pollfd descriptor{descriptor_, static_cast<short>(POLLIN | POLLHUP), 0};
            const auto polled = ::poll(&descriptor, 1, 20);
            if (polled < 0 && errno != EINTR) {
                readFailed = true;
            }

            if (polled > 0 && (descriptor.revents & POLLNVAL) != 0) {
                readFailed = true;
            }
            if (polled > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                for (;;) {
                    const auto count = ::read(descriptor_, bytes.data(), bytes.size());
                    if (count > 0) {
                        decoder.decode(bytes.data(), static_cast<std::size_t>(count), floats);
                        if (!floats.empty()) {
                            try {
                                samples_(floats.data(), floats.size());
                            } catch (...) {
                                callbackFailed = true;
                                break;
                            }
                        }
                        continue;
                    }
                    if (count == 0) {
                        reachedEof = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        readFailed = true;
                    }
                    break;
                }
            }

            if (stopRequested_ && steadyNowNs() >= drainDeadlineNs_) {
                break;
            }
        }

        const bool intentional = stopRequested_;
        running_ = false;
        if (!intentional && failure_) {
            try {
                failure_(Status::failure(
                    callbackFailed ? ErrorCode::internal_error : ErrorCode::io_error,
                    callbackFailed
                        ? "The microphone sample callback stopped unexpectedly."
                        : backend_ + " microphone capture stopped unexpectedly.",
                    "Check the microphone and desktop audio service."));
            } catch (...) {}
        }
    }

    std::string backend_;
    std::vector<std::string> fixedArguments_;
    std::mutex stopMutex_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<std::int64_t> drainDeadlineNs_{0};
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
        const auto sink = resolveSink();
        if (!sink) return sink.status();
        sinkTarget_ = sink.value();
        const auto current = readVolume();
        if (!current) {
            sinkTarget_.clear();
            return current.status();
        }

        previous_ = current.value();
        // Muted audio should stay muted and its hidden volume should not move.
        if (previous_.muted) {
            ducked_ = true;
            changed_ = false;
            return Status::success();
        }

        const float appliedLevel = static_cast<float>(
            std::lround(outputLevel * 100.0F)) / 100.0F;
        if (std::fabs(previous_.level - appliedLevel) <= 0.0001F) {
            ducked_ = true;
            changed_ = false;
            return Status::success();
        }

        const auto changed = setVolume(appliedLevel);
        if (!changed.ok()) {
            sinkTarget_.clear();
            return changed;
        }
        applied_ = {appliedLevel, false};
        changed_ = true;
        ducked_ = true;
        return Status::success();
    }

    void restore() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ducked_) return;
        if (changed_) {
            // Restore only while the sink still has the exact state LocalFlow
            // applied. A volume or mute change made during dictation wins.
            const auto current = readVolume();
            if (current && shouldRestoreDuckedVolume(applied_, current.value())) {
                (void)setVolume(previous_.level);
            }
        }
        ducked_ = false;
        changed_ = false;
        sinkTarget_.clear();
    }

private:
    Result<std::string> resolveSink() const {
        const bool wirePlumber = backend_.find("wpctl") != std::string::npos;
        const auto response = wirePlumber
            ? runCommand({"env", "LC_ALL=C", "wpctl", "inspect", "@DEFAULT_AUDIO_SINK@"})
            : runCommand({"pactl", "get-default-sink"});
        if (!response.launched || response.timedOut || response.exitCode != 0) {
            return Result<std::string>::failure(Status::failure(
                ErrorCode::io_error,
                wirePlumber
                    ? "Could not identify the default PipeWire output device."
                    : "Could not identify the default PulseAudio output device."));
        }
        return wirePlumber
            ? parseWpctlObjectId(response.standardOutput)
            : parseAudioServiceIdentifier(response.standardOutput);
    }

    Result<OutputVolumeState> readVolume() const {
        const bool wirePlumber = backend_.find("wpctl") != std::string::npos;
        if (wirePlumber) {
            const auto response = runCommand(
                {"env", "LC_ALL=C", "wpctl", "get-volume", sinkTarget_});
            if (!response.launched || response.timedOut || response.exitCode != 0) {
                return Result<OutputVolumeState>::failure(Status::failure(
                    ErrorCode::io_error, "Could not read the PipeWire output volume and mute state."));
            }
            return parseWpctlVolume(response.standardOutput);
        }

        const auto volume = runCommand(
            {"env", "LC_ALL=C", "pactl", "get-sink-volume", sinkTarget_});
        const auto mute = runCommand(
            {"env", "LC_ALL=C", "pactl", "get-sink-mute", sinkTarget_});
        if (!volume.launched || volume.timedOut || volume.exitCode != 0 ||
            !mute.launched || mute.timedOut || mute.exitCode != 0) {
            return Result<OutputVolumeState>::failure(Status::failure(
                ErrorCode::io_error, "Could not read the PulseAudio output volume and mute state."));
        }
        const auto parsedVolume = parsePactlVolume(volume.standardOutput);
        if (!parsedVolume) return Result<OutputVolumeState>::failure(parsedVolume.status());
        const auto parsedMute = parsePactlMute(mute.standardOutput);
        if (!parsedMute) return Result<OutputVolumeState>::failure(parsedMute.status());
        return Result<OutputVolumeState>::success({parsedVolume.value(), parsedMute.value()});
    }

    Status setVolume(float value) const {
        const auto percent = std::to_string(static_cast<int>(std::lround(value * 100.0F))) + "%";
        const auto response = backend_.find("wpctl") != std::string::npos
            ? runCommand({"wpctl", "set-volume", sinkTarget_, percent})
            : runCommand({"pactl", "set-sink-volume", sinkTarget_, percent});
        return response.launched && !response.timedOut && response.exitCode == 0
            ? Status::success()
            : Status::failure(ErrorCode::io_error, "Could not change output volume.");
    }

    std::string backend_;
    std::string sinkTarget_;
    std::mutex mutex_;
    OutputVolumeState previous_;
    OutputVolumeState applied_;
    bool ducked_{false};
    bool changed_{false};
};

}  // namespace

std::unique_ptr<AudioCaptureBackend> makeProcessAudioCapture(std::string backend) {
    return std::make_unique<ProcessCapture>(std::move(backend));
}

std::unique_ptr<AudioCaptureBackend> makeProcessAudioCaptureForTest(
    std::vector<std::string> command) {
    return std::make_unique<ProcessCapture>(
        "test recorder", std::move(command));
}

std::unique_ptr<AudioDucker> makeProcessAudioDucker(std::string backend) {
    return std::make_unique<ProcessDucker>(std::move(backend));
}

}  // namespace localflow::platform::linux::detail
