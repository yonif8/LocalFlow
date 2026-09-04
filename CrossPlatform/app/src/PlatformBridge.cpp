#include "PlatformBridge.hpp"

#include "LocalOcr.hpp"
#include "ScreenTermCaptureCoordinator.hpp"
#include "localflow/core/terminology.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include "localflow/windows/WindowsPlatform.hpp"
#include <UIAutomation.h>
#include <wrl/client.h>
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

float input_level(const float *samples, std::size_t count) {
  if (samples == nullptr || count == 0)
    return 0;
  double sum = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const double value = std::isfinite(samples[index]) ? samples[index] : 0.0;
    sum += value * value;
  }
  const double rms = std::sqrt(sum / double(count));
  const double db = 20.0 * std::log10(std::max(rms, 1e-7));
  return float(std::clamp((db + 50.0) / 50.0, 0.0, 1.0));
}

std::chrono::steady_clock::time_point bounded_event_time(
    std::chrono::steady_clock::time_point value) {
  const auto now = std::chrono::steady_clock::now();
  if (value == std::chrono::steady_clock::time_point{} || value > now ||
      now - value > std::chrono::seconds(5)) {
    return now;
  }
  return value;
}

std::chrono::steady_clock::time_point event_time_from_milliseconds(
    std::uint64_t milliseconds) {
  if (milliseconds == 0)
    return std::chrono::steady_clock::now();
  return bounded_event_time(std::chrono::steady_clock::time_point(
      std::chrono::milliseconds(milliseconds)));
}

void append_key_part(std::string &key, std::string_view value) {
  key += std::to_string(value.size());
  key.push_back(':');
  key.append(value.data(), value.size());
  key.push_back('|');
}
} // namespace

struct PlatformBridge::Implementation {
  Implementation() = default;

#ifndef _WIN32
  void refreshCapabilityReport() {
    lf_linux::SystemHostProbe probe;
    capabilities = lf_linux::CapabilityDetector().detect(probe);
  }
#endif

  PlatformConfiguration configuration;
  EventCallback callback;
  mutable std::mutex mutex;
  bool running = false;
  bool accepting = true;
  bool arming = false;
  bool recording = false;
  bool finishing = false;
  bool releasePending = false;
  bool pendingCancelled = false;
  std::uint64_t nextSession = 1;
  std::uint64_t activeSession = 0;
  std::chrono::steady_clock::time_point pressedAt{};
  std::chrono::steady_clock::time_point pendingReleasedAt{};
  std::string activeTrigger;
  std::vector<float> samples;
  std::uint32_t sampleRate = 16000;
  ScreenTermCaptureCoordinator screenTermCaptures;

#ifdef _WIN32
  using Target = localflow::windows::FocusedTextTargetIdentity;
  std::unique_ptr<localflow::windows::GlobalInputMonitor> shortcut;
  std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> audio;
  localflow::windows::SystemAudioDucker ducker;
  std::map<std::uint64_t, Target> targets;
  std::shared_ptr<localflow::windows::WindowsMediaOcr> ocr =
      std::make_shared<localflow::windows::WindowsMediaOcr>(1);
  std::vector<PlatformCapability> detectedCapabilities;
  bool requiredCapabilitiesReady = false;

  void refreshCapabilityReport() {
    std::error_code microphoneError;
    const auto microphoneDevices =
        localflow::windows::enumerate_capture_devices(microphoneError);
    const bool microphoneReady = !microphoneError && !microphoneDevices.empty();

    const HRESULT initialized =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsApartment = SUCCEEDED(initialized);
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    const HRESULT automationResult =
        (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE)
            ? CoCreateInstance(CLSID_CUIAutomation, nullptr,
                               CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&automation))
            : initialized;
    const bool automationReady =
        SUCCEEDED(automationResult) && automation.Get() != nullptr;
    automation.Reset();
    if (ownsApartment)
      CoUninitialize();

    detectedCapabilities = {
        {"shortcut", "Push-to-talk shortcut", "ready",
         "Windows global keyboard monitoring is available; the selected shortcut is verified when listening starts.",
         {}, true},
        {"microphone", "Microphone", microphoneReady ? "ready" : "blocked",
         microphoneReady
             ? "A Windows microphone input was detected. Recording permission is verified when listening starts."
             : microphoneError
                   ? "Windows could not enumerate microphone inputs: " +
                         microphoneError.message()
                   : "No enabled Windows microphone input was detected.",
         "Connect or enable a microphone and allow desktop apps to use it in Windows Privacy settings.",
         true},
        {"insertion", "Safe text insertion",
         automationReady ? "ready" : "blocked",
         automationReady
             ? "Windows UI Automation is available. The exact editable field is verified again before insertion."
             : "Windows UI Automation could not be initialized, so editable fields cannot be verified reliably.",
         "Restart LocalFlow outside the secure desktop. LocalFlow cannot insert into higher-privilege apps.",
         true},
        {"screen", "Screen-aware terminology", "ready",
         "On-device UI Automation text and OCR are available for optional screen terminology context.",
         {}, false},
    };
    requiredCapabilitiesReady = microphoneReady && automationReady;
  }

  static std::string utf8(const std::wstring &value) {
    if (value.empty())
      return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            int(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
      return {};
    std::string result(std::size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        int(value.size()), result.data(), bytes, nullptr,
                        nullptr);
    return result;
  }

  static std::uint32_t hotkey(const std::string &value) {
    if (value == "RightAlt")
      return VK_RMENU;
    if (value == "F8")
      return VK_F8;
    if (value == "F9")
      return VK_F9;
    return VK_RCONTROL;
  }

  static std::optional<localflow::windows::MouseButton>
  mouseButton(const std::string &value) {
    if (value == "middle")
      return localflow::windows::MouseButton::middle;
    if (value == "side1")
      return localflow::windows::MouseButton::x1;
    if (value == "side2")
      return localflow::windows::MouseButton::x2;
    return std::nullopt;
  }

  static std::string screenTargetKey(const Target &target) {
    std::string key = "windows|";
    append_key_part(key, std::to_string(target.window.process_id));
    append_key_part(
        key, std::to_string(reinterpret_cast<std::uintptr_t>(target.window.handle)));
    append_key_part(
        key, std::to_string(static_cast<unsigned>(target.fingerprint.backend)));
    append_key_part(key, std::to_string(target.fingerprint.foreground_window));
    append_key_part(key, std::to_string(target.fingerprint.native_focus_window));
    for (const auto component : target.fingerprint.automation_runtime_id)
      append_key_part(key, std::to_string(component));
    return key;
  }

  std::shared_future<std::vector<std::string>>
  beginScreenCapture(const Target &target) {
    if (!configuration.screenTerminology)
      return ready_terms();
    const auto recognizer = ocr;
    return screenTermCaptures.request(screenTargetKey(target), [target, recognizer]()
                                          -> std::optional<std::vector<std::string>> {
      auto accessibility =
          localflow::windows::capture_visible_accessibility_text(target);
      auto visibleStrings = std::move(accessibility.visibleStrings);
      std::error_code error;
      localflow::windows::GdiWindowCapture capture;
      auto frame = capture.capture(target.window, error);
      if (frame) {
        auto recognition = recognizer->recognize_async(std::move(*frame)).get();
        if (recognition.succeeded()) {
          visibleStrings.insert(
              visibleStrings.end(),
              std::make_move_iterator(recognition.lines.begin()),
              std::make_move_iterator(recognition.lines.end()));
        }
      }
      return localflow::core::ScreenTermExtractor::extract(visibleStrings);
    });
  }

  static std::string targetMessage(
      localflow::windows::FocusedTextTargetStatus status,
      const std::error_code &error = {}) {
    using Status = localflow::windows::FocusedTextTargetStatus;
    switch (status) {
    case Status::protected_content:
      return "LocalFlow discarded this press because a password or protected field is focused.";
    case Status::no_foreground_window:
    case Status::no_focused_control:
    case Status::not_editable:
      return "Place the cursor in an editable text field, then try again.";
    case Status::target_changed:
      return "The text cursor moved to a different field. Your transcript was not inserted.";
    case Status::automation_unavailable:
      return "LocalFlow could not safely verify this text field. Try another field or restart the target app.";
    case Status::invalid_target:
      return "The original text field is no longer available. Your transcript was not inserted.";
    case Status::system_error:
      return error ? "Windows could not verify the focused text field: " + error.message()
                   : "Windows could not verify the focused text field.";
    case Status::ready:
      return {};
    }
    return "Windows could not verify the focused text field.";
  }

  std::error_code startAudio() {
    if (!audio) {
      localflow::windows::AudioCaptureOptions options;
      if (!configuration.microphoneId.empty()) {
        const int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, configuration.microphoneId.data(),
            int(configuration.microphoneId.size()), nullptr, 0);
        if (count > 0) {
          std::wstring id(std::size_t(count), L'\0');
          MultiByteToWideChar(
              CP_UTF8, MB_ERR_INVALID_CHARS, configuration.microphoneId.data(),
              int(configuration.microphoneId.size()), id.data(), count);
          options.endpoint_id = std::move(id);
        }
      }
      audio = std::make_unique<localflow::windows::WasapiMicrophoneCapture>(
          std::move(options),
          [this](localflow::windows::AudioChunk chunk) {
            const float level =
                input_level(chunk.samples.data(), chunk.samples.size());
            std::uint64_t session = 0;
            {
              std::lock_guard lock(mutex);
              if (!running || (!arming && !recording))
                return;
              session = activeSession;
              if (sampleRate == 0)
                sampleRate = chunk.sample_rate_hz;
              if (chunk.sample_rate_hz == sampleRate) {
                samples.insert(samples.end(), chunk.samples.begin(),
                               chunk.samples.end());
              }
            }
            emit({PlatformEventKind::level,
                  session,
                  level,
                  {},
                  16000,
                  {},
                  {},
                  {}});
          },
          [this](std::error_code failure) {
            std::uint64_t session = 0;
            {
              std::lock_guard lock(mutex);
              if (!running || finishing)
                return;
              session = activeSession;
              accepting = false;
              arming = false;
              recording = false;
              finishing = false;
              releasePending = false;
              pendingCancelled = false;
              activeTrigger.clear();
              samples.clear();
            }
            emitTerminal({PlatformEventKind::error,
                          session,
                          0,
                          {},
                          16000,
                          {},
                          {},
                          failure.message()});
          });
    }
    return audio->start();
  }

  void handleShortcut(const localflow::windows::PttEvent &event) {
    if (event.kind == localflow::windows::PttEventKind::pressed)
      begin(event.timestamp, "windows");
    else if (event.kind == localflow::windows::PttEventKind::released)
      end(false, event.timestamp, "windows");
    else
      end(true, event.timestamp, "windows");
  }

  void begin(std::chrono::steady_clock::time_point shortcutTime,
             std::string triggerId) {
    std::uint64_t session = 0;
    {
      std::lock_guard lock(mutex);
      if (!running || !accepting || arming || recording || finishing)
        return;
      arming = true;
      activeSession = session = nextSession++;
      activeTrigger = std::move(triggerId);
      pressedAt = bounded_event_time(shortcutTime);
      releasePending = false;
      pendingCancelled = false;
      samples.clear();
      sampleRate = 0;
    }

    // Begin microphone startup before UI Automation. Samples remain in a
    // quarantine buffer until the exact non-secure field is verified, which
    // preserves the first syllable without ever processing unsafe presses.
    const auto audioError =
        configuration.keepMicrophoneWarm ? std::error_code{} : startAudio();
    if (audioError) {
      std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> failedAudio;
      bool notify = false;
      {
        std::lock_guard lock(mutex);
        if (activeSession == session) {
          notify = running;
          accepting = false;
          arming = false;
          releasePending = false;
          activeTrigger.clear();
          samples.clear();
          failedAudio = std::move(audio);
        }
      }
      if (failedAudio)
        failedAudio->stop();
      if (notify) {
        emitTerminal({PlatformEventKind::error,
                      session,
                      0,
                      {},
                      16000,
                      {},
                      {},
                      "Could not start the microphone: " +
                          audioError.message()});
      }
      return;
    }

    auto capture = localflow::windows::capture_focused_text_target();
    if (!capture.safe_for_insertion()) {
      std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> stoppingAudio;
      bool notify = false;
      {
        std::lock_guard lock(mutex);
        if (activeSession == session) {
          notify = running;
          accepting = false;
          arming = false;
          releasePending = false;
          pendingCancelled = false;
          activeTrigger.clear();
          samples.clear();
          if (!configuration.keepMicrophoneWarm)
            stoppingAudio = std::move(audio);
        }
      }
      if (stoppingAudio)
        stoppingAudio->stop();
      if (!notify)
        return;
      PlatformEvent rejected;
      rejected.kind = PlatformEventKind::rejected;
      rejected.sessionId = session;
      rejected.message = targetMessage(capture.status, capture.error);
      emitTerminal(std::move(rejected));
      return;
    }
    const Target target = *capture.target;
    bool activate = false;
    bool finishImmediately = false;
    bool cancelImmediately = false;
    std::chrono::steady_clock::time_point releasedAt{};
    {
      std::lock_guard lock(mutex);
      if (running && accepting && arming && activeSession == session) {
        targets[session] = target;
        arming = false;
        recording = true;
        activate = true;
        finishImmediately = releasePending;
        cancelImmediately = pendingCancelled;
        releasedAt = pendingReleasedAt;
        releasePending = false;
        pendingCancelled = false;
      }
    }
    if (!activate) {
      std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> stoppingAudio;
      {
        std::lock_guard lock(mutex);
        if (!configuration.keepMicrophoneWarm)
          stoppingAudio = std::move(audio);
        samples.clear();
      }
      if (stoppingAudio)
        stoppingAudio->stop();
      return;
    }
    auto terms = beginScreenCapture(target);
    if (configuration.duckAudio)
      (void)ducker.duck();
    PlatformEvent began;
    began.kind = PlatformEventKind::began;
    began.sessionId = session;
    began.targetAppId = utf8(target.window.application_id());
    began.screenTerms = std::move(terms);
    emit(std::move(began));
    if (finishImmediately)
      end(cancelImmediately, releasedAt, "windows");
  }

  void end(bool cancelled,
           std::chrono::steady_clock::time_point shortcutTime = {},
           const std::string &triggerId = {}) {
    std::unique_ptr<localflow::windows::WasapiMicrophoneCapture> stoppingAudio;
    PlatformEvent event;
    std::chrono::milliseconds held{0};
    {
      std::lock_guard lock(mutex);
      if (!triggerId.empty() && !activeTrigger.empty() &&
          triggerId != activeTrigger)
        return;
      if (arming && !recording) {
        releasePending = true;
        pendingCancelled = cancelled;
        pendingReleasedAt = bounded_event_time(shortcutTime);
        return;
      }
      if (!recording || finishing)
        return;
      // Close the admission gate in the same critical section that claims the
      // terminal transition. No subsequent press can slip in while native
      // cleanup runs or before the queued controller callback sees this event.
      accepting = false;
      finishing = true;
      event.sessionId = activeSession;
      const auto releasedAt = bounded_event_time(shortcutTime);
      held = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(releasedAt, pressedAt) - pressedAt);
      if (!configuration.keepMicrophoneWarm) {
        stoppingAudio = std::move(audio);
      }
    }
    if (stoppingAudio)
      stoppingAudio->stop();
    else if (!cancelled && held >= std::chrono::milliseconds(
                                      configuration.holdThresholdMs))
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    (void)ducker.restore();
    {
      std::lock_guard lock(mutex);
      recording = false;
      finishing = false;
      releasePending = false;
      pendingCancelled = false;
      activeTrigger.clear();
      event.sampleRate = sampleRate == 0 ? 16000 : sampleRate;
      event.samples = std::move(samples);
    }
    event.kind = cancelled || held < std::chrono::milliseconds(
                                         configuration.holdThresholdMs)
                     ? PlatformEventKind::cancelled
                     : PlatformEventKind::ended;
    emitTerminal(std::move(event));
  }

  bool insert(std::uint64_t session, const std::string &text,
              std::string *error) {
    Target expected;
    {
      std::lock_guard lock(mutex);
      if (!running) {
        if (error)
          *error = "This dictation session was cancelled.";
        return false;
      }
      const auto found = targets.find(session);
      if (found == targets.end()) {
        if (error)
          *error = "The original text field could not be verified safely.";
        return false;
      }
      expected = found->second;
    }
    localflow::windows::TextInsertionOptions options;
    options.clipboard_restore_delay =
        std::chrono::milliseconds(configuration.clipboardRestoreDelayMs);
    if (configuration.insertionMethod == "paste") {
      options.allow_unicode_fallback = false;
    } else if (configuration.insertionMethod == "type") {
      options.try_clipboard_paste = false;
    }
    localflow::windows::ForegroundTextInserter inserter(options);
    localflow::windows::TextInsertionOutcome outcome;
    const auto status = inserter.insert_utf8_into_focused_target(
        text, expected, &outcome);
    if (status && error) {
      *error = outcome.target_status ==
                       localflow::windows::FocusedTextTargetStatus::ready
                   ? status.message()
                   : targetMessage(outcome.target_status, outcome.target_error);
    }
    return !status;
  }

  std::string summary() const {
    return requiredCapabilitiesReady
               ? "Windows is ready for local dictation, safe field insertion, and optional on-device screen terminology."
               : "Windows needs attention before LocalFlow can dictate safely. Review the blocked capability below.";
  }

  std::vector<PlatformCapability> capabilityItems() const {
    return detectedCapabilities;
  }

  bool ready() const noexcept { return requiredCapabilitiesReady; }

  std::vector<PlatformMicrophone> microphones() {
    std::vector<PlatformMicrophone> result;
    std::error_code error;
    for (const auto &device :
         localflow::windows::enumerate_capture_devices(error)) {
      result.push_back(
          {utf8(device.id), utf8(device.display_name), device.is_default});
    }
    return result;
  }
#else
  using Target = lf_linux::ApplicationInfo;
  lf_linux::CapabilityReport capabilities;
  std::unique_ptr<lf_linux::GlobalShortcutBackend> shortcut;
  std::unique_ptr<lf_linux::GlobalShortcutBackend> mouseShortcut;
  std::unique_ptr<lf_linux::AudioCaptureBackend> audio;
  std::unique_ptr<lf_linux::AudioDucker> ducker;
  std::shared_ptr<lf_linux::FocusedTargetProvider> focusedTargets;
  std::shared_ptr<lf_linux::ScreenContextBackend> screen;
  std::shared_ptr<lf_linux::TextInsertionCoordinator> inserter;
  std::map<std::uint64_t, Target> targets;
  std::string resolvedMicrophoneId;

  static std::string linuxHotkey(const std::string &value) {
    if (value == "RightCtrl")
      return "Control_R";
    if (value == "RightAlt")
      return "Alt_R";
    return value.empty() ? "F8" : value;
  }

  static std::uint32_t linuxMouseButton(const std::string &value) {
    if (value == "middle")
      return 2;
    if (value == "side1")
      return 8;
    if (value == "side2")
      return 9;
    return 0;
  }

  static bool samePressApplication(
      const Target &pressTarget, const Target &screenTarget) {
    const bool hasProcessIds =
        pressTarget.processId > 0 && screenTarget.processId > 0;
    if (hasProcessIds && pressTarget.processId != screenTarget.processId)
      return false;
    const bool hasWindowTitles =
        !pressTarget.windowTitle.empty() && !screenTarget.windowTitle.empty();
    if (hasWindowTitles && pressTarget.windowTitle != screenTarget.windowTitle)
      return false;
    return hasProcessIds || hasWindowTitles;
  }

  static std::string screenTargetKey(
      const Target &pressTarget, const Target &screenTarget) {
    std::string key = "linux|";
    append_key_part(key, std::to_string(pressTarget.processId));
    append_key_part(key, pressTarget.applicationId);
    if (pressTarget.focusedTarget) {
      append_key_part(key, pressTarget.focusedTarget->busName);
      append_key_part(key, pressTarget.focusedTarget->objectPath);
      append_key_part(key, pressTarget.focusedTarget->accessibleId);
    }
    append_key_part(key, std::to_string(screenTarget.nativeWindowId));
    append_key_part(key, std::to_string(screenTarget.processId));
    append_key_part(key, screenTarget.applicationId);
    return key;
  }

  std::shared_future<std::vector<std::string>> beginScreenCapture(
      const Target &pressTarget, const Target &screenTarget) {
    if (!configuration.screenTerminology || !screen)
      return ready_terms();
    auto backend = screen;
    return screenTermCaptures.request(
        screenTargetKey(pressTarget, screenTarget),
        [backend, pressTarget, screenTarget]()
            -> std::optional<std::vector<std::string>> {
          auto accessibility =
              lf_linux::captureVisibleAccessibilityText(pressTarget);
          auto visibleStrings = std::move(accessibility.visibleStrings);
          auto captured = backend->captureContextFrame(screenTarget);
          if (captured) {
            auto frame = std::move(captured).value();
            LocalOcrFrame image;
            image.width = frame.width;
            image.height = frame.height;
            image.stride = frame.bytesPerRow;
            image.bgra = frame.pixelFormat == lf_linux::PixelFormat::bgra8;
            image.pixels = std::move(frame.pixels);
            auto recognition = LocalOcr::recognize(image);
            if (recognition.error.empty()) {
              visibleStrings.insert(
                  visibleStrings.end(),
                  std::make_move_iterator(recognition.lines.begin()),
                  std::make_move_iterator(recognition.lines.end()));
            }
          }
          return localflow::core::ScreenTermExtractor::extract(visibleStrings);
        });
  }

  lf_linux::Status startAudio() {
    if (!audio) {
      return lf_linux::Status::failure(
          lf_linux::ErrorCode::not_configured,
          "The microphone backend is unavailable.");
    }
    return audio->start(
        resolvedMicrophoneId, {16000, 1},
        [this](const float *data, std::size_t count) {
          const float level = input_level(data, count);
          std::uint64_t session = 0;
          {
            std::lock_guard lock(mutex);
            if (!running || (!arming && !recording))
              return;
            session = activeSession;
            samples.insert(samples.end(), data, data + count);
          }
          emit({PlatformEventKind::level,
                session,
                level,
                {},
                16000,
                {},
                {},
                {}});
        },
        [this](const lf_linux::Status &failure) {
          std::uint64_t session = 0;
          {
            std::lock_guard lock(mutex);
            if (!running || finishing)
              return;
            session = activeSession;
            accepting = false;
            arming = false;
            recording = false;
            finishing = false;
            releasePending = false;
            pendingCancelled = false;
            activeTrigger.clear();
            samples.clear();
          }
          emitTerminal({PlatformEventKind::error,
                        session,
                        0,
                        {},
                        16000,
                        {},
                        {},
                        failure.message});
        });
  }

  void handleShortcut(const lf_linux::ShortcutEvent &event) {
    if (event.edge == lf_linux::ShortcutEdge::pressed) {
      begin(event_time_from_milliseconds(event.monotonicTimestampMs), event.id);
    } else if (event.edge == lf_linux::ShortcutEdge::released) {
      end(false, event_time_from_milliseconds(event.monotonicTimestampMs),
          event.id);
    } else {
      end(true, event_time_from_milliseconds(event.monotonicTimestampMs),
          event.id);
    }
  }

  void begin(std::chrono::steady_clock::time_point shortcutTime,
             std::string triggerId) {
    std::uint64_t session = 0;
    {
      std::lock_guard lock(mutex);
      if (!running || !accepting || arming || recording || finishing)
        return;
      arming = true;
      activeSession = session = nextSession++;
      activeTrigger = std::move(triggerId);
      pressedAt = bounded_event_time(shortcutTime);
      releasePending = false;
      pendingCancelled = false;
      samples.clear();
      sampleRate = 16000;
    }

    const auto audioStatus = configuration.keepMicrophoneWarm
                                 ? lf_linux::Status::success()
                                 : startAudio();
    if (!audioStatus.ok()) {
      bool notify = false;
      {
        std::lock_guard lock(mutex);
        if (activeSession == session) {
          notify = running;
          accepting = false;
          arming = false;
          releasePending = false;
          activeTrigger.clear();
          samples.clear();
        }
      }
      if (notify) {
        emitTerminal({PlatformEventKind::error,
                      session,
                      0,
                      {},
                      16000,
                      {},
                      {},
                      audioStatus.message});
      }
      return;
    }

    lf_linux::Result<Target> capturedTarget = focusedTargets
        ? focusedTargets->snapshotFocusedTarget()
        : lf_linux::Result<Target>::failure(lf_linux::Status::failure(
              lf_linux::ErrorCode::not_configured,
              "LocalFlow could not access the focused text field.",
              "Enable desktop accessibility and restart LocalFlow."));
    lf_linux::Status targetStatus = capturedTarget
        ? lf_linux::validateFocusedTarget(
              capturedTarget.value(), capturedTarget.value())
        : capturedTarget.status();
    if (!targetStatus.ok()) {
      if (!configuration.keepMicrophoneWarm && audio)
        audio->stop();
      bool notify = false;
      {
        std::lock_guard lock(mutex);
        if (activeSession == session) {
          notify = running;
          accepting = false;
          arming = false;
          releasePending = false;
          pendingCancelled = false;
          activeTrigger.clear();
          samples.clear();
        }
      }
      if (!notify)
        return;
      PlatformEvent rejected;
      rejected.kind = PlatformEventKind::rejected;
      rejected.sessionId = session;
      rejected.message = targetStatus.message;
      if (!targetStatus.remediation.empty())
        rejected.message += " " + targetStatus.remediation;
      emitTerminal(std::move(rejected));
      return;
    }
    Target target = std::move(capturedTarget).value();
    std::optional<Target> screenTarget;
    if (configuration.screenTerminology && screen) {
      if (capabilities.session.type == lf_linux::SessionType::x11) {
        auto current = screen->activeApplication();
        if (current) {
          auto candidate = std::move(current).value();
          if (samePressApplication(target, candidate))
            screenTarget = std::move(candidate);
        }
      } else {
        screenTarget = target;
      }
    }
    bool activate = false;
    bool finishImmediately = false;
    bool cancelImmediately = false;
    std::chrono::steady_clock::time_point releasedAt{};
    std::string completedTrigger;
    {
      std::lock_guard lock(mutex);
      if (running && accepting && arming && activeSession == session) {
        targets[session] = target;
        arming = false;
        recording = true;
        activate = true;
        finishImmediately = releasePending;
        cancelImmediately = pendingCancelled;
        releasedAt = pendingReleasedAt;
        completedTrigger = activeTrigger;
        releasePending = false;
        pendingCancelled = false;
      }
    }
    if (!activate) {
      if (!configuration.keepMicrophoneWarm && audio)
        audio->stop();
      {
        std::lock_guard lock(mutex);
        samples.clear();
      }
      return;
    }
    if (configuration.duckAudio)
      (void)ducker->duck(0.2F);
    PlatformEvent began;
    began.kind = PlatformEventKind::began;
    began.sessionId = session;
    began.targetAppId = target.applicationId;
    began.screenTerms = screenTarget
                            ? beginScreenCapture(target, *screenTarget)
                            : ready_terms();
    emit(std::move(began));
    if (finishImmediately)
      end(cancelImmediately, releasedAt, completedTrigger);
  }

  void end(bool cancelled,
           std::chrono::steady_clock::time_point shortcutTime = {},
           const std::string &triggerId = {}) {
    PlatformEvent event;
    std::chrono::milliseconds held{0};
    {
      std::lock_guard lock(mutex);
      if (!triggerId.empty() && !activeTrigger.empty() &&
          triggerId != activeTrigger)
        return;
      if (arming && !recording) {
        releasePending = true;
        pendingCancelled = cancelled;
        pendingReleasedAt = bounded_event_time(shortcutTime);
        return;
      }
      if (!recording || finishing)
        return;
      accepting = false;
      finishing = true;
      event.sessionId = activeSession;
      const auto releasedAt = bounded_event_time(shortcutTime);
      held = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(releasedAt, pressedAt) - pressedAt);
    }
    if (!configuration.keepMicrophoneWarm)
      audio->stop();
    else if (!cancelled && held >= std::chrono::milliseconds(
                                      configuration.holdThresholdMs))
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ducker->restore();
    {
      std::lock_guard lock(mutex);
      recording = false;
      finishing = false;
      releasePending = false;
      pendingCancelled = false;
      activeTrigger.clear();
      event.sampleRate = sampleRate;
      event.samples = std::move(samples);
    }
    event.kind = cancelled || held < std::chrono::milliseconds(
                                         configuration.holdThresholdMs)
                     ? PlatformEventKind::cancelled
                     : PlatformEventKind::ended;
    emitTerminal(std::move(event));
  }

  bool insert(std::uint64_t session, const std::string &text,
              std::string *error) {
    Target expected;
    std::shared_ptr<lf_linux::TextInsertionCoordinator> coordinator;
    {
      std::lock_guard lock(mutex);
      if (!running || !inserter) {
        if (error)
          *error = "This dictation session was cancelled.";
        return false;
      }
      const auto found = targets.find(session);
      if (found == targets.end()) {
        if (error)
          *error = "The original target field could not be verified safely.";
        return false;
      }
      expected = found->second;
      coordinator = inserter;
    }
    auto result = coordinator->insert(text, expected);
    if (!result.ok() && error) {
      *error = result.status.message;
      if (!result.status.remediation.empty()) {
        *error += " " + result.status.remediation;
      }
    }
    return result.ok();
  }

  std::string summary() const {
    std::string result = std::string("Linux ") +
                         lf_linux::toString(capabilities.session.type) + ": ";
    bool first = true;
    for (const auto &capability : capabilities.capabilities) {
      if (!first)
        result += ", ";
      first = false;
      result += lf_linux::toString(capability.feature);
      result += "=";
      result += lf_linux::toString(capability.availability);
    }
    return result;
  }

  static std::string capabilityState(lf_linux::Availability availability) {
    switch (availability) {
    case lf_linux::Availability::available:
      return "ready";
    case lf_linux::Availability::permission_required:
      return "permission";
    case lf_linux::Availability::degraded:
      return "degraded";
    case lf_linux::Availability::unavailable:
    case lf_linux::Availability::unsupported:
      return "blocked";
    }
    return "blocked";
  }

  PlatformCapability capabilityItem(lf_linux::Feature feature,
                                    std::string id, std::string label,
                                    bool required) const {
    const auto *value = capabilities.find(feature);
    if (value == nullptr) {
      return {std::move(id), std::move(label), "blocked",
              "This capability could not be detected.",
              "Restart LocalFlow inside a supported graphical session.", required};
    }
    return {std::move(id), std::move(label),
            capabilityState(value->availability), value->detail,
            value->remediation, required};
  }

  std::vector<PlatformCapability> capabilityItems() const {
    auto shortcut = capabilityItem(lf_linux::Feature::global_shortcut,
                                   "shortcut", "Push-to-talk shortcut", true);
    auto microphone = capabilityItem(lf_linux::Feature::microphone_capture,
                                     "microphone", "Microphone", true);
    auto insertion = capabilityItem(lf_linux::Feature::accessibility_insertion,
                                    "insertion", "Safe text insertion", true);
    const auto *context =
        capabilities.find(lf_linux::Feature::accessibility_context);
    const auto *paste = capabilities.find(lf_linux::Feature::clipboard_paste);
    if (context == nullptr || !context->usable()) {
      insertion.state = "blocked";
      insertion.detail = context == nullptr
                             ? "The focused field cannot be verified safely."
                             : context->detail;
      insertion.remediation =
          context == nullptr ? "Install at-spi2-core and enable desktop accessibility."
                             : context->remediation;
    } else if (const auto *direct =
                   capabilities.find(lf_linux::Feature::accessibility_insertion);
               direct == nullptr || !direct->usable()) {
      if (paste != nullptr && paste->usable()) {
        insertion.state = capabilityState(paste->availability);
        insertion.detail = paste->detail;
        insertion.remediation = paste->remediation;
      }
    }
    auto screen = capabilityItem(lf_linux::Feature::screen_capture, "screen",
                                 "Screen-aware terminology", false);
    return {std::move(shortcut), std::move(microphone), std::move(insertion),
            std::move(screen)};
  }

  bool ready() const noexcept { return capabilities.canShipCoreDictation(); }

  std::vector<PlatformMicrophone> microphones() {
    auto backend = lf_linux::makeAudioCaptureBackend(capabilities);
    auto devices = backend->devices();
    if (!devices)
      return {};
    std::vector<PlatformMicrophone> result;
    for (const auto &device : devices.value()) {
      result.push_back({device.id, device.name, device.isDefault});
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
    if (copy)
      copy(std::move(event));
  }

  void emitTerminal(PlatformEvent event) {
    EventCallback copy;
    {
      std::lock_guard lock(mutex);
      accepting = false;
      copy = callback;
    }
    if (copy)
      copy(std::move(event));
  }

  bool start(const PlatformConfiguration &config, EventCallback next,
             std::string *error) {
    stop();
    refreshCapabilityReport();
    configuration = config;
    {
      std::lock_guard lock(mutex);
      callback = std::move(next);
      accepting = false;
    }
#ifdef _WIN32
    localflow::windows::InputMonitorOptions options;
    options.triggers = {
        localflow::windows::keyboard_trigger(hotkey(config.hotkey))};
    if (const auto button = mouseButton(config.mouseTrigger)) {
      options.triggers.push_back(localflow::windows::mouse_trigger(*button));
    }
    shortcut = std::make_unique<localflow::windows::GlobalInputMonitor>(
        std::move(options),
        [this](const auto &event) { handleShortcut(event); });

    // A warm capture must be fully ready before the global hook can admit a
    // press. Otherwise the first press after enabling listening can be lost or
    // race microphone startup.
    if (config.keepMicrophoneWarm) {
      const auto audioStatus = startAudio();
      if (audioStatus) {
        if (error)
          *error =
              "Could not keep the microphone ready: " + audioStatus.message();
        stop();
        return false;
      }
    }
    {
      std::lock_guard lock(mutex);
      running = true;
      accepting = true;
    }
    const auto status = shortcut->start();
    if (status) {
      if (error)
        *error = status.message();
      stop();
      return false;
    }
#else
    shortcut = lf_linux::makeGlobalShortcutBackend(capabilities);
    audio = lf_linux::makeAudioCaptureBackend(capabilities);
    ducker = lf_linux::makeAudioDucker(capabilities);
    focusedTargets = lf_linux::makeAtSpiFocusedTargetProvider(capabilities);
    resolvedMicrophoneId = config.microphoneId;
    if (!resolvedMicrophoneId.empty()) {
      const auto available = audio->devices();
      const bool stillConnected =
          available &&
          std::any_of(
              available.value().begin(), available.value().end(),
              [&](const auto &device) {
                return device.id == resolvedMicrophoneId;
              });
      if (!stillConnected)
        resolvedMicrophoneId.clear();
    }
    screen = std::shared_ptr<lf_linux::ScreenContextBackend>(
        lf_linux::makeScreenContextBackend(capabilities.session.type, {},
                                           focusedTargets)
            .release());
    lf_linux::InsertionOptions insertionOptions;
    insertionOptions.clipboardRestoreDelay =
        std::chrono::milliseconds(config.clipboardRestoreDelayMs);
    auto accessibility =
        config.insertionMethod == "paste"
            ? std::unique_ptr<lf_linux::AccessibilityTextInserter>{}
            : lf_linux::makeAtSpiTextInserter(capabilities);
    insertionOptions.allowClipboardFallback = config.insertionMethod != "type";
    inserter = std::make_shared<lf_linux::TextInsertionCoordinator>(
        std::move(accessibility), lf_linux::makeSystemClipboard(capabilities),
        lf_linux::makePasteInjector(capabilities), insertionOptions,
        focusedTargets);

    if (config.keepMicrophoneWarm) {
      const auto audioStatus = startAudio();
      if (!audioStatus.ok()) {
        if (error)
          *error = audioStatus.message;
        stop();
        return false;
      }
    }
    {
      std::lock_guard lock(mutex);
      running = true;
      accepting = true;
    }

    lf_linux::ShortcutSpec specification;
    specification.id = "push-to-talk-key";
    specification.trigger = linuxHotkey(config.hotkey);
    const auto status = shortcut->start(
        specification, [this](const auto &event) { handleShortcut(event); });
    if (!status.ok()) {
      if (error)
        *error = status.message +
                 (status.remediation.empty() ? "" : " " + status.remediation);
      stop();
      return false;
    }
    const auto configuredMouseButton = linuxMouseButton(config.mouseTrigger);
    if (configuredMouseButton != 0 &&
        capabilities.session.type == lf_linux::SessionType::x11) {
      mouseShortcut = lf_linux::makeGlobalShortcutBackend(capabilities);
      lf_linux::ShortcutSpec mouseSpecification;
      mouseSpecification.id = "push-to-talk-mouse";
      mouseSpecification.kind = lf_linux::ShortcutKind::mouse_button;
      mouseSpecification.mouseButton = configuredMouseButton;
      const auto mouseStatus = mouseShortcut->start(
          mouseSpecification,
          [this](const auto &event) { handleShortcut(event); });
      if (!mouseStatus.ok()) {
        if (error)
          *error = mouseStatus.message +
                   (mouseStatus.remediation.empty()
                        ? ""
                        : " " + mouseStatus.remediation);
        stop();
        return false;
      }
    }
#endif
    return true;
  }

  void stop() noexcept {
    {
      std::lock_guard lock(mutex);
      running = false;
      accepting = false;
    }
#ifdef _WIN32
    if (shortcut)
      shortcut->stop();
    screenTermCaptures.reset();
    if (audio)
      audio->stop();
    (void)ducker.restore();
    shortcut.reset();
    audio.reset();
#else
    if (mouseShortcut)
      mouseShortcut->stop();
    if (shortcut)
      shortcut->stop();
    screenTermCaptures.reset();
    if (screen)
      screen->cancelPendingCapture();
    if (audio)
      audio->stop();
    if (ducker)
      ducker->restore();
    shortcut.reset();
    mouseShortcut.reset();
    audio.reset();
    ducker.reset();
    inserter.reset();
    screen.reset();
    focusedTargets.reset();
    resolvedMicrophoneId.clear();
#endif
    std::lock_guard lock(mutex);
    arming = false;
    recording = false;
    finishing = false;
    releasePending = false;
    pendingCancelled = false;
    activeTrigger.clear();
    samples.clear();
    targets.clear();
    callback = {};
  }
};

PlatformBridge::PlatformBridge()
    : implementation_(std::make_unique<Implementation>()) {}
PlatformBridge::~PlatformBridge() { implementation_->stop(); }

bool PlatformBridge::start(const PlatformConfiguration &configuration,
                           EventCallback callback, std::string *error) {
  return implementation_->start(configuration, std::move(callback), error);
}

void PlatformBridge::stop() noexcept { implementation_->stop(); }

void PlatformBridge::setAcceptingInput(bool accepting) noexcept {
  std::lock_guard lock(implementation_->mutex);
  implementation_->accepting = accepting;
}

void PlatformBridge::cancelCurrentSession() noexcept {
  implementation_->end(true);
}

bool PlatformBridge::insert(std::uint64_t sessionId, const std::string &text,
                            std::string *error) {
  return implementation_->insert(sessionId, text, error);
}

void PlatformBridge::discardSession(std::uint64_t sessionId) noexcept {
  std::lock_guard lock(implementation_->mutex);
  implementation_->targets.erase(sessionId);
}

void PlatformBridge::refreshCapabilities() {
  implementation_->refreshCapabilityReport();
}

std::string PlatformBridge::capabilitySummary() const {
  return implementation_->summary();
}

std::vector<PlatformCapability> PlatformBridge::capabilities() const {
  return implementation_->capabilityItems();
}

bool PlatformBridge::readyForDictation() const noexcept {
  return implementation_->ready();
}

std::vector<PlatformMicrophone> PlatformBridge::microphones() {
  return implementation_->microphones();
}
