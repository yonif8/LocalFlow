#include "localflow/linux/LinuxPlatform.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
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
            "org.freedesktop.portal.ScreenCast",
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
    host.libraries = {"libX11.so.6", "libXtst.so.6", "libpulse.so.0"};
    host.executables = {"xclip", "pactl", "parec"};
    const auto report = CapabilityDetector{}.detect(host);
    EXPECT_EQ(report.session.type, SessionType::x11);
    EXPECT_EQ(report.find(Feature::global_shortcut)->availability, Availability::available);
    EXPECT_EQ(report.find(Feature::screen_capture)->availability, Availability::available);
    EXPECT_TRUE(report.find(Feature::clipboard_paste)->usable());
    EXPECT_EQ(report.find(Feature::microphone_capture)->availability, Availability::degraded);
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

class FakeAccessibility final : public AccessibilityTextInserter {
public:
    Status response;
    int calls{0};
    explicit FakeAccessibility(Status value) : response(std::move(value)) {}
    Status insertAtCaret(const std::string&) override { ++calls; return response; }
};

class FakeClipboard final : public Clipboard {
public:
    int snapshots{0};
    int writes{0};
    int restores{0};
    Result<ClipboardSnapshot> snapshot() override {
        ++snapshots;
        ClipboardSnapshot value;
        value.payloads["text/plain"] = {'o', 'l', 'd'};
        return Result<ClipboardSnapshot>::success(std::move(value));
    }
    Status setText(const std::string&) override { ++writes; return Status::success(); }
    Status restore(const ClipboardSnapshot&) override { ++restores; return Status::success(); }
};

class FakePaste final : public PasteInjector {
public:
    Status response;
    int calls{0};
    explicit FakePaste(Status value) : response(std::move(value)) {}
    Status paste() override { ++calls; return response; }
};

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

class FakeScreenPortal final : public ScreenCastPortal {
public:
    int sessions{0};
    int frames{0};
    int closes{0};
    Status ensureSession() override { ++sessions; return Status::success(); }
    Result<ScreenFrame> latestFrame() override {
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

void testPortalScreenSessionIsReused() {
    auto portal = std::make_shared<FakeScreenPortal>();
    {
        auto context = makeScreenContextBackend(SessionType::wayland, portal);
        EXPECT_TRUE(context->captureContextFrame().ok());
        EXPECT_TRUE(context->captureContextFrame().ok());
        EXPECT_EQ(portal->sessions, 1);
        EXPECT_EQ(portal->frames, 2);
    }
    EXPECT_EQ(portal->closes, 1);
}

}  // namespace

int main() {
    testWaylandCapabilities();
    testWaylandWithoutPortalsFailsClearly();
    testX11Capabilities();
    testPortalShortcutMapsBothEdges();
    testWaylandMouseShortcutRejected();
    testInsertionPrefersAccessibility();
    testInsertionRestoresClipboardAfterPasteFailure();
    testPortalScreenSessionIsReused();

    if (failures == 0) {
        std::cout << "All Linux platform tests passed.\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " Linux platform assertion(s) failed.\n";
    return EXIT_FAILURE;
}
