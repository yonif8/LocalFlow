#include "localflow/linux/LinuxPlatform.hpp"

#include "AudioSupport.hpp"
#include "InternalFactories.hpp"
#include "PortalSupport.hpp"
#include "Process.hpp"
#include "ShortcutState.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace localflow::platform::linux;

namespace {

int failures = 0;

#define EXPECT_TRUE(value) do { \
    if (!(value)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected true: " #value "\n"; \
        ++failures; \
    } \
} while (false)

#define EXPECT_EQ(left, right) do { \
    const auto actualLeft = (left); \
    const auto actualRight = (right); \
    if (!(actualLeft == actualRight)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << " expected equality: " #left " == " #right "\n"; \
        ++failures; \
    } \
} while (false)

class FakeHost final : public HostProbe {
public:
    std::map<std::string, std::string> environmentValues;
    std::set<std::string> executables;
    std::set<std::string> libraries;
    std::set<std::string> busNames;
    std::set<std::string> portalInterfaces;

    std::optional<std::string> environment(const std::string& name) const override {
        const auto found = environmentValues.find(name);
        return found == environmentValues.end()
            ? std::nullopt
            : std::optional<std::string>(found->second);
    }
    bool executableAvailable(const std::string& name) const override {
        return executables.count(name) != 0;
    }
    bool sharedLibraryAvailable(const std::vector<std::string>& names) const override {
        for (const auto& name : names) if (libraries.count(name) != 0) return true;
        return false;
    }
    bool sessionBusNameAvailable(const std::string& name) const override {
        return busNames.count(name) != 0;
    }
    bool portalInterfaceAvailable(const std::string& name) const override {
        return portalInterfaces.count(name) != 0;
    }
};

CapabilityReport detectWayland(bool portals) {
    FakeHost host;
    host.environmentValues["XDG_SESSION_TYPE"] = "wayland";
    host.environmentValues["WAYLAND_DISPLAY"] = "wayland-0";
    host.executables = {"wl-copy", "wl-paste", "pw-record", "wpctl"};
    host.libraries = {"libpipewire-0.3.so.0", "libatspi.so.0"};
    host.busNames = {"org.a11y.Bus"};
    if (portals) {
        host.portalInterfaces = {
            "org.freedesktop.portal.GlobalShortcuts",
            "org.freedesktop.portal.Screenshot",
            "org.freedesktop.portal.RemoteDesktop",
        };
    }
    return CapabilityDetector{}.detect(host);
}

void testWaylandCapabilities() {
    const auto report = detectWayland(true);
    EXPECT_EQ(report.session.type, SessionType::wayland);
    EXPECT_EQ(report.find(Feature::global_shortcut)->availability, Availability::permission_required);
    EXPECT_EQ(report.find(Feature::screen_capture)->availability, Availability::permission_required);
    EXPECT_EQ(report.find(Feature::active_application)->availability, Availability::degraded);
    EXPECT_TRUE(report.find(Feature::global_shortcut)->detail.find("press") != std::string::npos ||
                report.find(Feature::global_shortcut)->detail.find("Press") != std::string::npos);
    EXPECT_TRUE(report.canShipCoreDictation());
}

void testWaylandWithoutPortalsFailsClearly() {
    const auto report = detectWayland(false);
    EXPECT_EQ(report.find(Feature::global_shortcut)->availability, Availability::unavailable);
    EXPECT_EQ(report.find(Feature::screen_capture)->availability, Availability::unavailable);
    EXPECT_TRUE(!report.find(Feature::global_shortcut)->remediation.empty());
    EXPECT_TRUE(!report.find(Feature::screen_capture)->remediation.empty());
    EXPECT_TRUE(!report.canShipCoreDictation());
}

void testX11Capabilities() {
    FakeHost host;
    host.environmentValues["DISPLAY"] = ":0";
    host.libraries = {
        "libX11.so.6",
        "libXtst.so.6",
        "libpulse.so.0",
        "libatspi.so.0",
    };
    host.busNames = {"org.a11y.Bus"};
    host.executables = {"xclip", "pactl", "parec"};
    const auto report = CapabilityDetector{}.detect(host);
    EXPECT_EQ(report.session.type, SessionType::x11);
    EXPECT_EQ(report.find(Feature::global_shortcut)->availability, Availability::available);
    EXPECT_EQ(report.find(Feature::screen_capture)->availability, Availability::available);
    EXPECT_TRUE(report.find(Feature::clipboard_paste)->usable());
    EXPECT_EQ(report.find(Feature::microphone_capture)->availability, Availability::degraded);
}

void testClipboardCannotShipWithoutFocusedTargetVerification() {
    FakeHost host;
    host.environmentValues["DISPLAY"] = ":0";
    host.libraries = {"libX11.so.6", "libXtst.so.6", "libpulse.so.0"};
    host.executables = {"xclip", "pactl", "parec"};
    const auto report = CapabilityDetector{}.detect(host);
    EXPECT_EQ(
        report.find(Feature::clipboard_paste)->availability,
        Availability::unavailable);
    EXPECT_TRUE(
        report.find(Feature::clipboard_paste)->detail.find("AT-SPI2") !=
        std::string::npos);
    EXPECT_TRUE(!report.canShipCoreDictation());
}

class FakeShortcutPortal final : public GlobalShortcutsPortal {
public:
    Status result = Status::success();
    ShortcutCallback callback;
    int closes{0};
    Status bind(const ShortcutSpec&, ShortcutCallback value) override {
        callback = std::move(value);
        return result;
    }
    void close() noexcept override { ++closes; }
};

void testPortalShortcutMapsBothEdges() {
    auto portal = std::make_shared<FakeShortcutPortal>();
    auto report = detectWayland(true);
    auto backend = makeGlobalShortcutBackend(report, portal);
    std::vector<ShortcutEdge> edges;
    const auto status = backend->start(
        {"push-to-talk", ShortcutKind::key, "F8", {}, 0},
        [&](const ShortcutEvent& event) { edges.push_back(event.edge); });
    EXPECT_TRUE(status.ok());
    portal->callback({"push-to-talk", ShortcutEdge::pressed, 1});
    portal->callback({"push-to-talk", ShortcutEdge::released, 2});
    EXPECT_EQ(edges.size(), std::size_t{2});
    EXPECT_EQ(edges[0], ShortcutEdge::pressed);
    EXPECT_EQ(edges[1], ShortcutEdge::released);
    backend->stop();
    EXPECT_EQ(portal->closes, 1);
}

void testNativeShortcutCancellationState() {
    detail::PushToTalkShortcutState state;
    auto edge = state.handle(detail::ShortcutInput::escape_pressed);
    EXPECT_TRUE(!edge.has_value());

    edge = state.handle(detail::ShortcutInput::trigger_pressed);
    EXPECT_TRUE(edge.has_value());
    if (edge) EXPECT_EQ(*edge, ShortcutEdge::pressed);
    EXPECT_TRUE(state.held());

    // Auto-repeat cannot start another recording.
    EXPECT_TRUE(!state.handle(detail::ShortcutInput::trigger_pressed).has_value());
    edge = state.handle(detail::ShortcutInput::escape_pressed);
    EXPECT_TRUE(edge.has_value());
    if (edge) EXPECT_EQ(*edge, ShortcutEdge::cancelled);
    EXPECT_TRUE(!state.held());

    // The physical PTT release after Escape must not end a new session.
    EXPECT_TRUE(!state.handle(detail::ShortcutInput::trigger_released).has_value());
    edge = state.handle(detail::ShortcutInput::trigger_pressed);
    EXPECT_TRUE(edge.has_value());
    edge = state.handle(detail::ShortcutInput::trigger_released);
    EXPECT_TRUE(edge.has_value());
    if (edge) EXPECT_EQ(*edge, ShortcutEdge::released);
}

void testPactlCaptureDeviceParsing() {
    const std::string sources =
        "Source #41\n"
        "\tState: RUNNING\n"
        "\tName: alsa_input.usb-Rode_NT_USB-00.mono-fallback\n"
        "\tDescription: RODE NT-USB Analog Mono\n"
        "\tMonitor of Sink: n/a\n"
        "Source #42\n"
        "\tName: alsa_output.pci-0000_00_1f.3.analog-stereo.monitor\n"
        "\tDescription: Monitor of Built-in Audio\n"
        "\tMonitor of Sink: 12\n"
        "Source #43\n"
        "\tName: bluez_input.11_22_33_44_55_66.headset-head-unit\n"
        "\tDescription: Family Headset\n"
        "\tMonitor of Sink: n/a\n";
    const auto devices = detail::parsePactlSources(
        sources,
        "bluez_input.11_22_33_44_55_66.headset-head-unit\n");
    EXPECT_EQ(devices.size(), std::size_t{2});
    if (devices.size() == 2) {
        EXPECT_EQ(devices[0].id, std::string("alsa_input.usb-Rode_NT_USB-00.mono-fallback"));
        EXPECT_EQ(devices[0].name, std::string("RODE NT-USB Analog Mono"));
        EXPECT_TRUE(!devices[0].isDefault);
        EXPECT_EQ(devices[1].name, std::string("Family Headset"));
        EXPECT_TRUE(devices[1].isDefault);
    }
    const auto monitorsOnly = detail::parsePactlSources(
        "Source #9\n"
        "\tName: virtual_output.capture\n"
        "\tDescription: Virtual output capture\n"
        "\tMonitor of Sink: 8\n",
        "virtual_output.capture\n");
    EXPECT_TRUE(monitorsOnly.empty());
}

void testAudioServiceParsersAndSafeDuckingDecision() {
    const auto wpctl = detail::parseWpctlVolume("Volume: 0.420000 [MUTED]\n");
    EXPECT_TRUE(wpctl.ok());
    if (wpctl) {
        EXPECT_TRUE(std::fabs(wpctl.value().level - 0.42F) < 0.0001F);
        EXPECT_TRUE(wpctl.value().muted);
    }
    const auto wpctlId = detail::parseWpctlObjectId(
        "id 73, type PipeWire:Interface:Node\n"
        "    node.name = \"alsa_output.usb-DAC\"\n");
    EXPECT_TRUE(wpctlId.ok());
    if (wpctlId) EXPECT_EQ(wpctlId.value(), std::string("73"));
    const auto pulseId = detail::parseAudioServiceIdentifier(
        "alsa_output.pci-0000_00_1f.3.analog-stereo\n");
    EXPECT_TRUE(pulseId.ok());
    EXPECT_TRUE(!detail::parseAudioServiceIdentifier("two sink names\n").ok());

    const auto pactlVolume = detail::parsePactlVolume(
        "Volume: front-left: 32768 / 50% / -18.06 dB, front-right: 32768 / 50% / -18.06 dB\n");
    EXPECT_TRUE(pactlVolume.ok());
    if (pactlVolume) EXPECT_TRUE(std::fabs(pactlVolume.value() - 0.5F) < 0.0001F);
    const auto pactlMuted = detail::parsePactlMute("Mute: yes\n");
    EXPECT_TRUE(pactlMuted.ok());
    if (pactlMuted) EXPECT_TRUE(pactlMuted.value());
    EXPECT_TRUE(!detail::parsePactlMute("unknown").ok());

    const detail::OutputVolumeState applied{0.2F, false};
    EXPECT_TRUE(detail::shouldRestoreDuckedVolume(applied, {0.2F, false}));
    EXPECT_TRUE(!detail::shouldRestoreDuckedVolume(applied, {0.3F, false}));
    EXPECT_TRUE(!detail::shouldRestoreDuckedVolume(applied, {0.2F, true}));
}

void testPcmDecoderPreservesSplitSample() {
    detail::S16LePcmDecoder decoder;
    std::vector<float> samples;
    const std::uint8_t first[]{0x34};
    decoder.decode(first, sizeof(first), samples);
    EXPECT_TRUE(samples.empty());

    const std::uint8_t second[]{0x12, 0x00};
    decoder.decode(second, sizeof(second), samples);
    EXPECT_EQ(samples.size(), std::size_t{1});
    if (!samples.empty()) {
        EXPECT_TRUE(std::fabs(samples[0] - (4660.0F / 32768.0F)) < 0.0001F);
    }

    const std::uint8_t third[]{0x80};
    decoder.decode(third, sizeof(third), samples);
    EXPECT_EQ(samples.size(), std::size_t{1});
    if (!samples.empty()) EXPECT_EQ(samples[0], -1.0F);
}

void testIntentionalStopDrainsRecorderTailWithinBound() {
    if (!detail::executableOnPath("python3")) return;

    const std::string program =
        "import os, signal, time\n"
        "def stopped(signum, frame):\n"
        "    os.write(1, b'\\x00\\x40')\n"
        "    raise SystemExit(0)\n"
        "signal.signal(signal.SIGINT, stopped)\n"
        "os.write(1, b'\\x00\\x00')\n"
        "while True:\n"
        "    time.sleep(1)\n";
    auto capture = detail::makeProcessAudioCaptureForTest(
        {"python3", "-c", program});
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<float> received;
    bool failed = false;
    const auto started = capture->start(
        "fake-device", {16000, 1},
        [&](const float* samples, std::size_t count) {
            std::lock_guard<std::mutex> lock(mutex);
            received.insert(received.end(), samples, samples + count);
            changed.notify_all();
        },
        [&](const Status&) {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            changed.notify_all();
        });
    EXPECT_TRUE(started.ok());
    if (!started.ok()) return;

    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(
            lock, std::chrono::seconds(2), [&] { return !received.empty() || failed; }));
    }
    const auto beforeStop = std::chrono::steady_clock::now();
    capture->stop();
    const auto stopDuration = std::chrono::steady_clock::now() - beforeStop;

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_TRUE(!failed);
    EXPECT_TRUE(stopDuration < std::chrono::seconds(1));
    EXPECT_EQ(received.size(), std::size_t{2});
    if (received.size() == 2) {
        EXPECT_EQ(received[0], 0.0F);
        EXPECT_EQ(received[1], 0.5F);
    }
}

void testWaylandMouseShortcutRejected() {
    auto portal = std::make_shared<FakeShortcutPortal>();
    auto backend = makeGlobalShortcutBackend(detectWayland(true), portal);
    ShortcutSpec shortcut;
    shortcut.kind = ShortcutKind::mouse_button;
    shortcut.mouseButton = 8;
    const auto status = backend->start(shortcut, [](const ShortcutEvent&) {});
    EXPECT_EQ(status.code, ErrorCode::unsupported_session);
    EXPECT_TRUE(!status.remediation.empty());
}

class BlockingShortcutPortal final : public GlobalShortcutsPortal {
public:
    Status bind(const ShortcutSpec&, ShortcutCallback) override {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return closed; });
        return Status::failure(ErrorCode::cancelled, "cancelled");
    }
    void close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        ++closes;
        closed = true;
        changed.notify_all();
    }
    void waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool closed{false};
    int closes{0};
};

void testPortalShortcutSetupCanBeCancelled() {
    auto portal = std::make_shared<BlockingShortcutPortal>();
    auto backend = makeGlobalShortcutBackend(detectWayland(true), portal);
    auto starting = std::async(std::launch::async, [&] {
        return backend->start(
            {"push-to-talk", ShortcutKind::key, "F8", {}, 0},
            [](const ShortcutEvent&) {});
    });
    portal->waitUntilEntered();
    backend->stop();
    const auto status = starting.get();
    EXPECT_EQ(status.code, ErrorCode::cancelled);
    EXPECT_EQ(portal->closes, 1);
}

void testPortalResponseDiagnostics() {
    EXPECT_TRUE(detail::portalResponseStatus(0, "screenshot").ok());
    EXPECT_EQ(
        detail::portalResponseStatus(1, "screenshot").code,
        ErrorCode::cancelled);
    const auto denied = detail::portalResponseStatus(
        2, "keyboard control", "approve it");
    EXPECT_EQ(denied.code, ErrorCode::permission_denied);
    EXPECT_EQ(denied.remediation, std::string("approve it"));
    EXPECT_EQ(
        detail::portalResponseStatus(99, "screenshot").code,
        ErrorCode::protocol_error);
}

void testPortalShortcutTriggerUsesXdgSyntax() {
    ShortcutSpec shortcut;
    shortcut.trigger = "F8";
    shortcut.modifiers = {Modifier::control, Modifier::shift};
    EXPECT_EQ(
        detail::portalShortcutTrigger(shortcut),
        std::string("CTRL+SHIFT+F8"));
}

class FakeAccessibility final : public AccessibilityTextInserter {
public:
    Status response;
    int calls{0};
    int expectedTargetCalls{0};
    explicit FakeAccessibility(Status value) : response(std::move(value)) {}
    Status insertAtCaret(const std::string&) override { ++calls; return response; }
    Status insertAtCaret(
        const std::string&,
        const ApplicationInfo&) override {
        ++calls;
        ++expectedTargetCalls;
        return response;
    }
};

class FakeClipboard final : public Clipboard {
public:
    int snapshots{0};
    int writes{0};
    int restores{0};
    std::string lastText;
    Result<ClipboardSnapshot> snapshot() override {
        ++snapshots;
        ClipboardSnapshot value;
        value.payloads["text/plain"] = {'o', 'l', 'd'};
        return Result<ClipboardSnapshot>::success(std::move(value));
    }
    Status setText(const std::string& text) override {
        ++writes;
        lastText = text;
        return Status::success();
    }
    Status restore(const ClipboardSnapshot&) override { ++restores; return Status::success(); }
};

class FakePaste final : public PasteInjector {
public:
    Status response;
    int calls{0};
    explicit FakePaste(Status value) : response(std::move(value)) {}
    Status paste() override { ++calls; return response; }
};

ApplicationInfo focusedTarget(
    std::string objectPath = "/org/a11y/atspi/accessible/42",
    FieldSecurity security = FieldSecurity::non_secure,
    bool editable = true,
    bool focused = true) {
    ApplicationInfo result;
    result.name = "Example Editor";
    result.applicationId = "org.example.Editor";
    result.windowTitle = "Document";
    result.processId = 4242;
    result.focusedTarget = FocusedAccessibleTarget{
        ":1.42",
        std::move(objectPath),
        "editor-body",
        "text",
        focused,
        editable,
        security,
    };
    return result;
}

class FakeFocusedTargets final : public FocusedTargetProvider {
public:
    std::vector<ApplicationInfo> snapshots;
    Status failure;
    int calls{0};

    Result<ApplicationInfo> snapshotFocusedTarget() override {
        ++calls;
        if (!failure.ok()) return Result<ApplicationInfo>::failure(failure);
        if (snapshots.empty()) {
            return Result<ApplicationInfo>::failure(Status::failure(
                ErrorCode::service_unavailable, "No fake target."));
        }
        const auto index = std::min<std::size_t>(
            static_cast<std::size_t>(calls - 1), snapshots.size() - 1);
        return Result<ApplicationInfo>::success(snapshots[index]);
    }
};

void testFocusedTargetValidationIsExactAndFailClosed() {
    const auto expected = focusedTarget();
    EXPECT_TRUE(validateFocusedTarget(expected, expected).ok());

    auto changed = expected;
    changed.focusedTarget->objectPath = "/org/a11y/atspi/accessible/43";
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::focus_changed);

    changed = expected;
    changed.processId = 4243;
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::focus_changed);

    changed = expected;
    changed.focusedTarget->security = FieldSecurity::secure;
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::secure_field);

    changed = expected;
    changed.focusedTarget->security = FieldSecurity::unknown;
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::secure_field);

    changed = expected;
    changed.focusedTarget->editable = false;
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::not_editable);

    changed = expected;
    changed.focusedTarget.reset();
    EXPECT_EQ(
        validateFocusedTarget(expected, changed).code,
        ErrorCode::not_configured);
}

void testVerifiedTargetIsPassedToAccessibilityInsertion() {
    const auto expected = focusedTarget();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {expected};
    auto accessibility = std::make_unique<FakeAccessibility>(Status::success());
    auto* accessibilityRaw = accessibility.get();
    auto clipboard = std::make_unique<FakeClipboard>();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("hello", expected);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(targets->calls, 1);
    EXPECT_EQ(accessibilityRaw->expectedTargetCalls, 1);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testFocusChangeRefusesPasteAndCopiesRecovery() {
    const auto expected = focusedTarget();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {focusedTarget("/org/a11y/atspi/accessible/99")};
    auto accessibility = std::make_unique<FakeAccessibility>(Status::success());
    auto* accessibilityRaw = accessibility.get();
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("safe recovery", expected);
    EXPECT_EQ(result.status.code, ErrorCode::focus_changed);
    EXPECT_EQ(result.backend, std::string("clipboard recovery"));
    EXPECT_EQ(accessibilityRaw->calls, 0);
    EXPECT_EQ(pasteRaw->calls, 0);
    EXPECT_EQ(clipboardRaw->writes, 1);
    EXPECT_EQ(clipboardRaw->lastText, std::string("safe recovery"));
}

void testSecureTargetNeverCopiesOrPastes() {
    const auto expected = focusedTarget();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {
        focusedTarget(
            "/org/a11y/atspi/accessible/42",
            FieldSecurity::secure),
    };
    auto accessibility = std::make_unique<FakeAccessibility>(Status::success());
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("never expose this", expected);
    EXPECT_EQ(result.status.code, ErrorCode::secure_field);
    EXPECT_EQ(clipboardRaw->writes, 0);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testMissingExpectedTargetNeverPastes() {
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {focusedTarget()};
    auto accessibility = std::make_unique<FakeAccessibility>(Status::success());
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("hello");
    EXPECT_EQ(result.status.code, ErrorCode::not_configured);
    EXPECT_EQ(targets->calls, 0);
    EXPECT_EQ(clipboardRaw->writes, 0);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testExpectedTargetWithoutVerifierCannotFallBackToPaste() {
    const auto expected = focusedTarget();
    auto accessibility = std::make_unique<FakeAccessibility>(Status::failure(
        ErrorCode::not_editable, "Direct insertion unavailable."));
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)});

    const auto result = coordinator.insert("hello", expected);
    EXPECT_EQ(result.status.code, ErrorCode::not_configured);
    EXPECT_EQ(clipboardRaw->writes, 0);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testFocusRaceImmediatelyBeforePasteLeavesRecoveryCopy() {
    const auto expected = focusedTarget();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {
        expected,
        focusedTarget("/org/a11y/atspi/accessible/88"),
    };
    auto accessibility = std::make_unique<FakeAccessibility>(Status::failure(
        ErrorCode::not_editable, "Direct insertion unavailable."));
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("keep this", expected);
    EXPECT_EQ(result.status.code, ErrorCode::focus_changed);
    EXPECT_EQ(result.backend, std::string("clipboard recovery"));
    EXPECT_EQ(targets->calls, 2);
    EXPECT_EQ(clipboardRaw->writes, 1);
    EXPECT_EQ(clipboardRaw->restores, 0);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testSecureRaceBeforePasteRestoresClipboard() {
    const auto expected = focusedTarget();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {
        expected,
        focusedTarget(
            "/org/a11y/atspi/accessible/42",
            FieldSecurity::secure),
    };
    auto accessibility = std::make_unique<FakeAccessibility>(Status::failure(
        ErrorCode::not_editable, "Direct insertion unavailable."));
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    auto* pasteRaw = paste.get();
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)}, targets);

    const auto result = coordinator.insert("do not paste", expected);
    EXPECT_EQ(result.status.code, ErrorCode::secure_field);
    EXPECT_EQ(targets->calls, 2);
    EXPECT_EQ(clipboardRaw->writes, 1);
    EXPECT_EQ(clipboardRaw->restores, 1);
    EXPECT_EQ(pasteRaw->calls, 0);
}

void testInsertionPrefersAccessibility() {
    auto accessibility = std::make_unique<FakeAccessibility>(Status::success());
    auto* accessibilityRaw = accessibility.get();
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::success());
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)});
    const auto result = coordinator.insert("hello");
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.backend, std::string("AT-SPI2 EditableText"));
    EXPECT_EQ(accessibilityRaw->calls, 1);
    EXPECT_EQ(clipboardRaw->snapshots, 0);
}

void testInsertionRestoresClipboardAfterPasteFailure() {
    auto accessibility = std::make_unique<FakeAccessibility>(Status::failure(
        ErrorCode::not_editable, "not editable"));
    auto clipboard = std::make_unique<FakeClipboard>();
    auto* clipboardRaw = clipboard.get();
    auto paste = std::make_unique<FakePaste>(Status::failure(
        ErrorCode::permission_denied, "paste denied"));
    TextInsertionCoordinator coordinator(
        std::move(accessibility), std::move(clipboard), std::move(paste),
        {true, std::chrono::milliseconds(0)});
    const auto result = coordinator.insert("hello");
    EXPECT_TRUE(!result.ok());
    EXPECT_EQ(result.status.code, ErrorCode::permission_denied);
    EXPECT_EQ(clipboardRaw->writes, 1);
    EXPECT_EQ(clipboardRaw->restores, 1);
}

class FakeScreenPortal final : public ScreenshotPortal {
public:
    int frames{0};
    int closes{0};
    Result<ScreenFrame> captureFrame() override {
        ++frames;
        ScreenFrame frame;
        frame.width = 1;
        frame.height = 1;
        frame.bytesPerRow = 4;
        frame.pixels = {0, 0, 0, 255};
        return Result<ScreenFrame>::success(std::move(frame));
    }
    void close() noexcept override { ++closes; }
};

void testPortalScreenshotCapturesEachContextFrame() {
    auto portal = std::make_shared<FakeScreenPortal>();
    {
        auto context = makeScreenContextBackend(SessionType::wayland, portal);
        EXPECT_TRUE(context->captureContextFrame().ok());
        EXPECT_TRUE(context->captureContextFrame().ok());
        EXPECT_EQ(portal->frames, 2);
    }
    EXPECT_EQ(portal->closes, 1);
}

void testWaylandScreenContextReturnsAtSpiApplicationInfo() {
    auto portal = std::make_shared<FakeScreenPortal>();
    auto targets = std::make_shared<FakeFocusedTargets>();
    targets->snapshots = {focusedTarget()};
    auto context = makeScreenContextBackend(
        SessionType::wayland, portal, targets);
    const auto application = context->activeApplication();
    EXPECT_TRUE(application.ok());
    if (application) {
        EXPECT_EQ(application.value().processId, std::int64_t{4242});
        EXPECT_EQ(
            application.value().applicationId,
            std::string("org.example.Editor"));
        EXPECT_TRUE(application.value().focusedTarget.has_value());
    }
    EXPECT_EQ(targets->calls, 1);
}

class FakeRemoteDesktopPortal final : public RemoteDesktopPortal {
public:
    Status ensureResult = Status::success();
    Status sendResult = Status::success();
    int sessions{0};
    int closes{0};
    std::vector<std::pair<std::uint32_t, bool>> keys;

    Status ensureKeyboardSession() override {
        ++sessions;
        return ensureResult;
    }
    Status sendKeysym(std::uint32_t keysym, bool pressed) override {
        keys.emplace_back(keysym, pressed);
        return sendResult;
    }
    void close() noexcept override { ++closes; }
};

void testPortalPasteUsesBalancedControlV() {
    auto portal = std::make_shared<FakeRemoteDesktopPortal>();
    {
        auto paste = makePasteInjector(detectWayland(true), portal);
        EXPECT_TRUE(paste->paste().ok());
        EXPECT_EQ(portal->sessions, 1);
        EXPECT_EQ(portal->keys.size(), std::size_t{4});
        EXPECT_EQ(portal->keys[0], std::make_pair(std::uint32_t{0xffe3}, true));
        EXPECT_EQ(portal->keys[1], std::make_pair(std::uint32_t{0x0076}, true));
        EXPECT_EQ(portal->keys[2], std::make_pair(std::uint32_t{0x0076}, false));
        EXPECT_EQ(portal->keys[3], std::make_pair(std::uint32_t{0xffe3}, false));
    }
    EXPECT_EQ(portal->closes, 1);
}

}  // namespace

int main() {
    testWaylandCapabilities();
    testWaylandWithoutPortalsFailsClearly();
    testX11Capabilities();
    testClipboardCannotShipWithoutFocusedTargetVerification();
    testPortalShortcutMapsBothEdges();
    testNativeShortcutCancellationState();
    testPactlCaptureDeviceParsing();
    testAudioServiceParsersAndSafeDuckingDecision();
    testPcmDecoderPreservesSplitSample();
    testIntentionalStopDrainsRecorderTailWithinBound();
    testWaylandMouseShortcutRejected();
    testPortalShortcutSetupCanBeCancelled();
    testPortalResponseDiagnostics();
    testPortalShortcutTriggerUsesXdgSyntax();
    testFocusedTargetValidationIsExactAndFailClosed();
    testVerifiedTargetIsPassedToAccessibilityInsertion();
    testFocusChangeRefusesPasteAndCopiesRecovery();
    testSecureTargetNeverCopiesOrPastes();
    testMissingExpectedTargetNeverPastes();
    testExpectedTargetWithoutVerifierCannotFallBackToPaste();
    testFocusRaceImmediatelyBeforePasteLeavesRecoveryCopy();
    testSecureRaceBeforePasteRestoresClipboard();
    testInsertionPrefersAccessibility();
    testInsertionRestoresClipboardAfterPasteFailure();
    testPortalScreenshotCapturesEachContextFrame();
    testWaylandScreenContextReturnsAtSpiApplicationInfo();
    testPortalPasteUsesBalancedControlV();

    if (failures == 0) {
        std::cout << "All Linux platform tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " Linux platform assertion(s) failed.\n";
    return EXIT_FAILURE;
}
