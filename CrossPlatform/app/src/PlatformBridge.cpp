#include "PlatformBridge.hpp"

#include "LocalOcr.hpp"
#include "localflow/core/terminology.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include "localflow/windows/WindowsPlatform.hpp"
#else
#include "localflow/linux/LinuxPlatform.hpp"
namespace lf_linux = localflow::platform::linux;
#endif

namespace {
std::shared_future<std::vector<std::string>> ready_terms() {
    std::promise<std::vector<std::string>> promise;
    auto future = promise.get_future().share();
    promise.set_value({});
    return future;
}

float input_level(const float* samples, std::size_t count) {
    if (samples == nullptr || count == 0) return 0;
    double sum = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const double value = std::isfinite(samples[index]) ? samples[index] : 0.0;
        sum += value * value;
    }
    const double rms = std::sqrt(sum / double(count));
    const double db = 20.0 * std::log10(std::max(rms, 1e-7));
    return float(std::clamp((db + 50.0) / 50.0, 0.0, 1.0));
}

template <typename Work>
std::shared_future<std::vector<std::string>> detached_terms(Work work) {
    auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
    auto future = promise->get_future().share();
    try {
        std::thread([promise, work = std::move(work)]() mutable {
            try {
                promise->set_value(work());
            } catch (...) {
                try { promise->set_value({}); } catch (...) {}
            }
        }).detach();
    } catch (...) {
        promise->set_value({});
    }
    return future;
}
}

struct PlatformBridge::Implementation {
    PlatformConfiguration configuration;
    EventCallback callback;
    mutable std::mutex mutex;
    bool running = false;
    bool accepting = true;
    bool recording = false;
    std::uint64_t nextSession = 1;
    std::uint64_t activeSession = 0;
    std::chrono::steady_clock::time_point pressedAt{};
    std::vector<float> samples;
    std::uint32_t sampleRate = 16000;

#ifdef _WIN32
    using Target = localflow::windows::ForegroundWindowIdentity;
    std::unique_ptr<localflow::windows::GlobalInputMonitor> shortcut;
    std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> audio;
    localflow::windows::SystemAudioDucker ducker;
    std::map<std::uint64_t, Target> targets;

    static std::string utf8(const std::wstring& value) {
        if (value.empty()) return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            int(value.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return {};
        std::string result(std::size_t(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()),
            result.data(), bytes, nullptr, nullptr);
        return result;
    }

    static std::uint32_t hotkey(const std::string& value) {
        if (value == "RightAlt") return VK_RMENU;
        if (value == "F8") return VK_F8;
        if (value == "F9") return VK_F9;
        return VK_RCONTROL;
    }

    std::shared_future<std::vector<std::string>> beginScreenCapture(const Target& target) {
        if (!configuration.screenTerminology) return ready_terms();
        return detached_terms([target] {
            std::error_code error;
            localflow::windows::GdiWindowCapture capture;
            auto frame = capture.capture(target, error);
            if (!frame) return std::vector<std::string>{};
            localflow::windows::WindowsMediaOcr ocr;
            auto recognition = ocr.recognize_async(std::move(*frame)).get();
            if (!recognition.succeeded()) return std::vector<std::string>{};
            return localflow::core::ScreenTermExtractor::extract(recognition.lines);
        });
    }

    void handleShortcut(const localflow::windows::PttEvent& event) {
        if (event.kind == localflow::windows::PttEventKind::pressed) begin();
        else if (event.kind == localflow::windows::PttEventKind::released) end(false);
        else end(true);
    }

    void begin() {
        std::error_code targetError;
        auto target = localflow::windows::query_foreground_window(targetError);
        std::uint64_t session = 0;
        {
            std::lock_guard lock(mutex);
            if (!running || !accepting || recording) return;
            recording = true;
            activeSession = session = nextSession++;
            pressedAt = std::chrono::steady_clock::now();
            samples.clear();
            sampleRate = 0;
            if (target) targets[session] = *target;
        }
        auto terms = target ? beginScreenCapture(*target) : ready_terms();
        localflow::windows::AudioCaptureOptions options;
        if (!configuration.microphoneId.empty()) {
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                configuration.microphoneId.data(), int(configuration.microphoneId.size()), nullptr, 0);
            if (count > 0) {
                std::wstring id(std::size_t(count), L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configuration.microphoneId.data(),
                    int(configuration.microphoneId.size()), id.data(), count);
                options.endpoint_id = std::move(id);
            }
        }
        audio = std::make_unique<localflow::windows::WasapiMicrophoneCapture>(
            std::move(options),
            [this, session](localflow::windows::AudioChunk chunk) {
                float level = input_level(chunk.samples.data(), chunk.samples.size());
                {
                    std::lock_guard lock(mutex);
                    if (!recording || activeSession != session) return;
                    if (sampleRate == 0) sampleRate = chunk.sample_rate_hz;
                    if (chunk.sample_rate_hz == sampleRate) {
                        samples.insert(samples.end(), chunk.samples.begin(), chunk.samples.end());
                    }
                }
                emit({PlatformEventKind::level, session, level});
            },
            [this, session](std::error_code error) {
                emit({PlatformEventKind::error, session, 0, {}, 16000, {}, {}, error.message()});
            });
        const auto audioError = audio->start();
        if (audioError) {
            {
                std::lock_guard lock(mutex);
                recording = false;
            }
            audio.reset();
            emit({PlatformEventKind::error, session, 0, {}, 16000, {}, {},
                "Could not start the microphone: " + audioError.message()});
            return;
        }
        if (configuration.duckAudio) (void)ducker.duck();
        PlatformEvent began;
        began.kind = PlatformEventKind::began;
        began.sessionId = session;
        began.targetAppId = target ? utf8(target->application_id()) : std::string{};
        began.screenTerms = std::move(terms);
        emit(std::move(began));
    }

    void end(bool cancelled) {
        std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> stoppingAudio;
        PlatformEvent event;
        std::chrono::milliseconds held{0};
        {
            std::lock_guard lock(mutex);
            if (!recording) return;
            recording = false;
            event.sessionId = activeSession;
            held = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - pressedAt);
            stoppingAudio = std::move(audio);
        }
        if (stoppingAudio) stoppingAudio->stop();
        (void)ducker.restore();
        {
            std::lock_guard lock(mutex);
            event.sampleRate = sampleRate == 0 ? 16000 : sampleRate;
            event.samples = std::move(samples);
        }
        event.kind = cancelled || held < std::chrono::milliseconds(100)
            ? PlatformEventKind::cancelled : PlatformEventKind::ended;
        emit(std::move(event));
    }

    bool insert(std::uint64_t session, const std::string& text, std::string* error) {
        std::optional<Target> expected;
        {
            std::lock_guard lock(mutex);
            const auto found = targets.find(session);
            if (found != targets.end()) expected = found->second;
        }
        localflow::windows::TextInsertionOptions options;
        options.clipboard_restore_delay = std::chrono::milliseconds(configuration.clipboardRestoreDelayMs);
        localflow::windows::ForegroundTextInserter inserter(options);
        localflow::windows::TextInsertionOutcome outcome;
        const auto status = inserter.insert_utf8(text, expected, &outcome);
        if (status && error) *error = status.message();
        return !status;
    }

    std::string summary() const {
        return "Windows: global shortcut, WASAPI microphone, local Windows OCR, and safe foreground insertion are available.";
    }
#else
    using Target = lf_linux::ApplicationInfo;
    lf_linux::CapabilityReport capabilities;
    std::unique_ptr<lf_linux::GlobalShortcutBackend> shortcut;
    std::unique_ptr<lf_linux::AudioCaptureBackend> audio;
    std::unique_ptr<lf_linux::AudioDucker> ducker;
    std::shared_ptr<lf_linux::ScreenContextBackend> screen;
    std::unique_ptr<lf_linux::TextInsertionCoordinator> inserter;
    std::map<std::uint64_t, Target> targets;

    static std::string linuxHotkey(const std::string& value) {
        if (value == "RightCtrl") return "Control_R";
        if (value == "RightAlt") return "Alt_R";
        return value.empty() ? "F8" : value;
    }

    std::shared_future<std::vector<std::string>> beginScreenCapture() {
        if (!configuration.screenTerminology || !screen) return ready_terms();
        auto backend = screen;
        return detached_terms([backend] {
            auto captured = backend->captureContextFrame();
            if (!captured) return std::vector<std::string>{};
            auto frame = std::move(captured).value();
            LocalOcrFrame image;
            image.width = frame.width;
            image.height = frame.height;
            image.stride = frame.bytesPerRow;
            image.bgra = frame.pixelFormat == lf_linux::PixelFormat::bgra8;
            image.pixels = std::move(frame.pixels);
            auto recognition = LocalOcr::recognize(image);
            if (!recognition.error.empty()) return std::vector<std::string>{};
            return localflow::core::ScreenTermExtractor::extract(recognition.lines);
        });
    }

    void handleShortcut(const lf_linux::ShortcutEvent& event) {
        if (event.edge == lf_linux::ShortcutEdge::pressed) begin();
        else end(false);
    }

    void begin() {
        std::optional<Target> target;
        if (screen) {
            auto current = screen->activeApplication();
            if (current) target = std::move(current).value();
        }
        std::uint64_t session = 0;
        {
            std::lock_guard lock(mutex);
            if (!running || !accepting || recording) return;
            recording = true;
            activeSession = session = nextSession++;
            pressedAt = std::chrono::steady_clock::now();
            samples.clear();
            sampleRate = 16000;
            if (target) targets[session] = *target;
        }
        const auto status = audio->start(
            configuration.microphoneId,
            {16000, 1},
            [this, session](const float* data, std::size_t count) {
                const float level = input_level(data, count);
                {
                    std::lock_guard lock(mutex);
                    if (!recording || activeSession != session) return;
                    samples.insert(samples.end(), data, data + count);
                }
                emit({PlatformEventKind::level, session, level});
            },
            [this, session](const lf_linux::Status& failure) {
                emit({PlatformEventKind::error, session, 0, {}, 16000, {}, {}, failure.message});
            });
        if (!status.ok()) {
            {
                std::lock_guard lock(mutex);
                recording = false;
            }
            emit({PlatformEventKind::error, session, 0, {}, 16000, {}, {}, status.message});
            return;
        }
        if (configuration.duckAudio) (void)ducker->duck(0.2F);
        PlatformEvent began;
        began.kind = PlatformEventKind::began;
        began.sessionId = session;
        began.targetAppId = target ? target->applicationId : std::string{};
        began.screenTerms = beginScreenCapture();
        emit(std::move(began));
    }

    void end(bool cancelled) {
        PlatformEvent event;
        std::chrono::milliseconds held{0};
        {
            std::lock_guard lock(mutex);
            if (!recording) return;
            recording = false;
            event.sessionId = activeSession;
            held = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - pressedAt);
        }
        audio->stop();
        ducker->restore();
        {
            std::lock_guard lock(mutex);
            event.sampleRate = sampleRate;
            event.samples = std::move(samples);
        }
        event.kind = cancelled || held < std::chrono::milliseconds(100)
            ? PlatformEventKind::cancelled : PlatformEventKind::ended;
        emit(std::move(event));
    }

    bool insert(std::uint64_t session, const std::string& text, std::string* error) {
        std::optional<Target> expected;
        {
            std::lock_guard lock(mutex);
            const auto found = targets.find(session);
            if (found != targets.end()) expected = found->second;
        }
        if (expected && screen) {
            auto current = screen->activeApplication();
            if (current) {
                const auto& value = current.value();
                const bool samePid = expected->processId > 0 && value.processId == expected->processId;
                const bool sameApp = !expected->applicationId.empty()
                    && value.applicationId == expected->applicationId;
                if (!samePid && !sameApp) {
                    if (error) *error = "The focused app changed while LocalFlow was processing.";
                    return false;
                }
            }
        }
        auto result = inserter->insert(text);
        if (!result.ok() && error) *error = result.status.message;
        return result.ok();
    }

    std::string summary() const {
        std::string result = std::string("Linux ") + lf_linux::toString(capabilities.session.type) + ": ";
        bool first = true;
        for (const auto& capability : capabilities.capabilities) {
            if (!first) result += ", ";
            first = false;
            result += lf_linux::toString(capability.feature);
            result += "=";
            result += lf_linux::toString(capability.availability);
        }
        return result;
    }
#endif

    void emit(PlatformEvent event) {
        EventCallback copy;
        {
            std::lock_guard lock(mutex);
            copy = callback;
        }
        if (copy) copy(std::move(event));
    }

    bool start(const PlatformConfiguration& config, EventCallback next, std::string* error) {
        stop();
        configuration = config;
        callback = std::move(next);
        accepting = true;
#ifdef _WIN32
        localflow::windows::InputMonitorOptions options;
        options.triggers = {localflow::windows::keyboard_trigger(hotkey(config.hotkey))};
        shortcut = std::make_unique<localflow::windows::GlobalInputMonitor>(
            std::move(options), [this](const auto& event) { handleShortcut(event); });
        const auto status = shortcut->start();
        if (status) {
            if (error) *error = status.message();
            shortcut.reset();
            return false;
        }
#else
        lf_linux::SystemHostProbe probe;
        capabilities = lf_linux::CapabilityDetector().detect(probe);
        shortcut = lf_linux::makeGlobalShortcutBackend(capabilities);
        audio = lf_linux::makeAudioCaptureBackend(capabilities);
        ducker = lf_linux::makeAudioDucker(capabilities);
        screen = std::shared_ptr<lf_linux::ScreenContextBackend>(
            lf_linux::makeScreenContextBackend(capabilities.session.type).release());
        lf_linux::InsertionOptions insertionOptions;
        insertionOptions.clipboardRestoreDelay = std::chrono::milliseconds(config.clipboardRestoreDelayMs);
        inserter = std::make_unique<lf_linux::TextInsertionCoordinator>(
            lf_linux::makeAtSpiTextInserter(capabilities),
            lf_linux::makeSystemClipboard(capabilities),
            lf_linux::makePasteInjector(capabilities),
            insertionOptions);
        lf_linux::ShortcutSpec specification;
        specification.trigger = linuxHotkey(config.hotkey);
        const auto status = shortcut->start(specification, [this](const auto& event) { handleShortcut(event); });
        if (!status.ok()) {
            if (error) *error = status.message + (status.remediation.empty() ? "" : " " + status.remediation);
            shortcut.reset();
            return false;
        }
#endif
        {
            std::lock_guard lock(mutex);
            running = true;
        }
        return true;
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex);
            running = false;
            accepting = false;
        }
#ifdef _WIN32
        if (shortcut) shortcut->stop();
        if (audio) audio->stop();
        (void)ducker.restore();
        shortcut.reset();
        audio.reset();
#else
        if (shortcut) shortcut->stop();
        if (audio) audio->stop();
        if (ducker) ducker->restore();
        shortcut.reset();
        audio.reset();
        ducker.reset();
        inserter.reset();
        screen.reset();
#endif
        std::lock_guard lock(mutex);
        recording = false;
        samples.clear();
        targets.clear();
        callback = {};
    }
};

PlatformBridge::PlatformBridge() : implementation_(std::make_unique<Implementation>()) {}
PlatformBridge::~PlatformBridge() { implementation_->stop(); }

bool PlatformBridge::start(
    const PlatformConfiguration& configuration, EventCallback callback, std::string* error) {
    return implementation_->start(configuration, std::move(callback), error);
}

void PlatformBridge::stop() noexcept { implementation_->stop(); }

void PlatformBridge::setAcceptingInput(bool accepting) noexcept {
    std::lock_guard lock(implementation_->mutex);
    implementation_->accepting = accepting;
}

bool PlatformBridge::insert(std::uint64_t sessionId, const std::string& text, std::string* error) {
    return implementation_->insert(sessionId, text, error);
}

void PlatformBridge::discardSession(std::uint64_t sessionId) noexcept {
    std::lock_guard lock(implementation_->mutex);
    implementation_->targets.erase(sessionId);
}

std::string PlatformBridge::capabilitySummary() const { return implementation_->summary(); }
