#include "localflow/linux/Capabilities.hpp"

#include "Process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dlfcn.h>

#ifndef LOCALFLOW_LINUX_WITH_QT_PORTALS
#define LOCALFLOW_LINUX_WITH_QT_PORTALS 0
#endif

#ifndef LOCALFLOW_LINUX_WITH_QT_CLIPBOARD
#define LOCALFLOW_LINUX_WITH_QT_CLIPBOARD 0
#endif

#if LOCALFLOW_LINUX_WITH_QT_PORTALS
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QString>
#endif

namespace localflow::platform::linux {
namespace {

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string environmentOrEmpty(const HostProbe& host, const std::string& name) {
    const auto value = host.environment(name);
    return value.has_value() ? *value : std::string{};
}

bool isSandboxed(const HostProbe& host) {
    return host.environment("FLATPAK_ID").has_value() ||
           host.environment("SNAP").has_value() ||
           host.environment("APPIMAGE").has_value();
}

Capability makeCapability(
    Feature feature,
    Availability availability,
    std::string backend,
    std::string detail,
    std::string remediation = {}) {
    return {
        feature,
        availability,
        std::move(backend),
        std::move(detail),
        std::move(remediation),
    };
}

}  // namespace

const Capability* CapabilityReport::find(Feature feature) const noexcept {
    const auto found = std::find_if(
        capabilities.begin(), capabilities.end(), [feature](const Capability& capability) {
            return capability.feature == feature;
        });
    return found == capabilities.end() ? nullptr : &*found;
}

bool CapabilityReport::canShipCoreDictation() const noexcept {
    const auto usable = [this](Feature feature) {
        const auto* capability = find(feature);
        return capability != nullptr && capability->usable();
    };
    return usable(Feature::global_shortcut) &&
           usable(Feature::microphone_capture) &&
           usable(Feature::accessibility_context) &&
           (usable(Feature::accessibility_insertion) || usable(Feature::clipboard_paste));
}

std::optional<std::string> SystemHostProbe::environment(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

bool SystemHostProbe::executableAvailable(const std::string& name) const {
    return detail::executableOnPath(name);
}

bool SystemHostProbe::sharedLibraryAvailable(
    const std::vector<std::string>& sonames) const {
    for (const auto& soname : sonames) {
        void* handle = ::dlopen(soname.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (handle != nullptr) {
            ::dlclose(handle);
            return true;
        }
    }
    return false;
}

bool SystemHostProbe::sessionBusNameAvailable(const std::string& busName) const {
#if LOCALFLOW_LINUX_WITH_QT_PORTALS
    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || bus.interface() == nullptr) return false;
    const QDBusReply<bool> registered = bus.interface()->isServiceRegistered(
        QString::fromStdString(busName));
    return registered.isValid() && registered.value();
#else
    if (!executableAvailable("gdbus")) {
        return false;
    }
    const auto response = detail::runCommand({
        "gdbus",
        "call",
        "--session",
        "--dest",
        "org.freedesktop.DBus",
        "--object-path",
        "/org/freedesktop/DBus",
        "--method",
        "org.freedesktop.DBus.NameHasOwner",
        busName,
    });
    return response.launched && !response.timedOut && response.exitCode == 0 &&
           response.standardOutput.find("true") != std::string::npos;
#endif
}

bool SystemHostProbe::portalInterfaceAvailable(
    const std::string& interfaceName) const {
    if (!sessionBusNameAvailable("org.freedesktop.portal.Desktop")) {
        return false;
    }
#if LOCALFLOW_LINUX_WITH_QT_PORTALS
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    message << QString::fromStdString(interfaceName) << QStringLiteral("version");
    const auto response = QDBusConnection::sessionBus().call(
        message, QDBus::Block, 2000);
    return response.type() == QDBusMessage::ReplyMessage &&
           response.arguments().size() == 1;
#else
    const auto response = detail::runCommand({
        "gdbus",
        "call",
        "--session",
        "--dest",
        "org.freedesktop.portal.Desktop",
        "--object-path",
        "/org/freedesktop/portal/desktop",
        "--method",
        "org.freedesktop.DBus.Properties.Get",
        interfaceName,
        "version",
    });
    return response.launched && !response.timedOut && response.exitCode == 0;
#endif
}

CapabilityReport CapabilityDetector::detect(const HostProbe& host) const {
    CapabilityReport report;
    report.session.desktop = environmentOrEmpty(host, "XDG_CURRENT_DESKTOP");
    report.session.display = environmentOrEmpty(host, "DISPLAY");
    report.session.waylandDisplay = environmentOrEmpty(host, "WAYLAND_DISPLAY");
    report.session.sandboxed = isSandboxed(host);

    const auto declaredSession = lowercase(environmentOrEmpty(host, "XDG_SESSION_TYPE"));
    if (declaredSession == "wayland" || !report.session.waylandDisplay.empty()) {
        report.session.type = SessionType::wayland;
    } else if (declaredSession == "x11" || !report.session.display.empty()) {
        report.session.type = SessionType::x11;
    }

    const bool x11 = host.sharedLibraryAvailable({"libX11.so.6", "libX11.so"});
    const bool xTest = host.sharedLibraryAvailable({"libXtst.so.6", "libXtst.so"}) ||
                       host.executableAvailable("xdotool");
    const bool atSpi = host.sharedLibraryAvailable({"libatspi.so.0", "libatspi.so"}) &&
                       host.sessionBusNameAvailable("org.a11y.Bus");
    const bool globalShortcutsPortal = host.portalInterfaceAvailable(
        "org.freedesktop.portal.GlobalShortcuts");
    const bool screenshotPortal = host.portalInterfaceAvailable(
        "org.freedesktop.portal.Screenshot");
    const bool remoteDesktopPortal = host.portalInterfaceAvailable(
        "org.freedesktop.portal.RemoteDesktop");
    const bool pipeWire = host.sharedLibraryAvailable({"libpipewire-0.3.so.0"}) ||
                          host.executableAvailable("pw-record");
    const bool pulse = host.sharedLibraryAvailable({"libpulse.so.0"}) ||
                       host.executableAvailable("parec");
    const bool waylandClipboard = host.executableAvailable("wl-copy") &&
                                  host.executableAvailable("wl-paste");
    const bool x11Clipboard = host.executableAvailable("xclip") ||
                              host.executableAvailable("xsel");
    constexpr bool qtClipboard = LOCALFLOW_LINUX_WITH_QT_CLIPBOARD != 0;

    if (report.session.type == SessionType::wayland) {
        report.capabilities.push_back(globalShortcutsPortal
            ? makeCapability(
                  Feature::global_shortcut,
                  Availability::permission_required,
                  "xdg-desktop-portal GlobalShortcuts",
                  "Press and release events are available after compositor consent.",
                  "Approve LocalFlow's shortcut when the desktop asks.")
            : makeCapability(
                  Feature::global_shortcut,
                  Availability::unavailable,
                  "Wayland",
                  "The compositor does not expose the GlobalShortcuts portal.",
                  "Use current GNOME or KDE with xdg-desktop-portal, or sign in to an X11 session."));

        report.capabilities.push_back(makeCapability(
            Feature::active_application,
            atSpi ? Availability::degraded : Availability::unsupported,
            atSpi ? "AT-SPI focused target" : "Wayland",
            atSpi
                ? "A bounded accessibility snapshot provides the focused object's exact bus/path identity and application PID."
                : "Wayland intentionally has no universal foreground-window API.",
            atSpi
                ? "Applications that do not expose AT-SPI metadata are rejected as unverifiable."
                : "Enable the desktop accessibility bus for safe target verification."));

        report.capabilities.push_back(screenshotPortal
            ? makeCapability(
                  Feature::screen_capture,
                  Availability::permission_required,
                  "xdg-desktop-portal Screenshot",
                  "A compositor-mediated screenshot supplies pixels without leaving a screen-sharing session active.",
                  "Approve screenshot access when the desktop asks.")
            : makeCapability(
                  Feature::screen_capture,
                  Availability::unavailable,
                  "Wayland",
                  "The desktop does not expose the Screenshot portal.",
                  "Install xdg-desktop-portal and the portal backend for your desktop."));
    } else if (report.session.type == SessionType::x11) {
        report.capabilities.push_back(x11
            ? makeCapability(
                  Feature::global_shortcut,
                  Availability::available,
                  "XGrabKey/XGrabButton",
                  "Native X11 grabs provide deterministic press and release events.")
            : makeCapability(
                  Feature::global_shortcut,
                  Availability::unavailable,
                  "X11",
                  "libX11 is not installed.",
                  "Install the X11 client libraries."));

        report.capabilities.push_back(x11
            ? makeCapability(
                  Feature::active_application,
                  Availability::available,
                  "EWMH",
                  "The active window is read through _NET_ACTIVE_WINDOW.")
            : makeCapability(
                  Feature::active_application,
                  Availability::unavailable,
                  "X11",
                  "libX11 is not installed."));

        report.capabilities.push_back(x11
            ? makeCapability(
                  Feature::screen_capture,
                  Availability::available,
                  "X11 active-window capture",
                  "The active X11 window can be captured without a recurring chooser.")
            : makeCapability(
                  Feature::screen_capture,
                  Availability::unavailable,
                  "X11",
                  "libX11 is not installed."));
    } else {
        const auto recovery = "Start LocalFlow inside an X11 or Wayland graphical session.";
        report.capabilities.push_back(makeCapability(
            Feature::global_shortcut,
            Availability::unsupported,
            "none",
            "No supported graphical session was detected.",
            recovery));
        report.capabilities.push_back(makeCapability(
            Feature::active_application,
            Availability::unsupported,
            "none",
            "No supported graphical session was detected.",
            recovery));
        report.capabilities.push_back(makeCapability(
            Feature::screen_capture,
            Availability::unsupported,
            "none",
            "No supported graphical session was detected.",
            recovery));
    }

    report.capabilities.push_back(atSpi
        ? makeCapability(
              Feature::accessibility_context,
              Availability::available,
              "AT-SPI2",
              "Focused accessible text and application metadata can supplement OCR.")
        : makeCapability(
              Feature::accessibility_context,
              Availability::unavailable,
              "AT-SPI2",
              "The accessibility bus or libatspi is unavailable.",
              "Install at-spi2-core and enable desktop accessibility."));

    report.capabilities.push_back(atSpi
        ? makeCapability(
              Feature::accessibility_insertion,
              Availability::available,
              "AT-SPI2 EditableText",
              "Text can be inserted only after the exact focused, editable, non-secure object is revalidated.")
        : makeCapability(
              Feature::accessibility_insertion,
              Availability::unavailable,
              "AT-SPI2",
              "The accessibility bus or libatspi is unavailable.",
              "Install at-spi2-core and enable desktop accessibility."));

    if (report.session.type == SessionType::x11) {
        report.capabilities.push_back(
            (qtClipboard || x11Clipboard) && xTest && atSpi
            ? makeCapability(
                  Feature::clipboard_paste,
                  Availability::available,
                  qtClipboard
                      ? "Qt QClipboard + XTest"
                      : "X11 command clipboard + XTest",
                  qtClipboard
                      ? "All advertised clipboard MIME formats are transactionally restored after paste."
                      : "Plain-text clipboard paste is available with degraded restoration fidelity.")
            : makeCapability(
                  Feature::clipboard_paste,
                  Availability::unavailable,
                  "X11",
                  !atSpi
                      ? "Clipboard paste is disabled because the exact focused field cannot be verified without AT-SPI2."
                      : "A clipboard provider or safe XTest paste injector is missing.",
                  !atSpi
                      ? "Install at-spi2-core and enable desktop accessibility."
                      : qtClipboard
                      ? "Install libXtst."
                      : "Install xclip (or xsel) and libXtst."));
    } else if (report.session.type == SessionType::wayland) {
        report.capabilities.push_back(
            (qtClipboard || waylandClipboard) && remoteDesktopPortal && atSpi
            ? makeCapability(
                  Feature::clipboard_paste,
                  Availability::permission_required,
                  qtClipboard
                      ? "Qt QClipboard + RemoteDesktop portal"
                      : "Wayland command clipboard + RemoteDesktop portal",
                  qtClipboard
                      ? "All advertised clipboard MIME formats are transactionally restored; paste injection still requires compositor approval."
                      : "Plain-text clipboard access is degraded; paste injection requires compositor approval.",
                  "Approve keyboard control when LocalFlow asks. No uinput helper is used.")
            : makeCapability(
                  Feature::clipboard_paste,
                  Availability::unavailable,
                  "Wayland",
                  !atSpi
                      ? "Clipboard paste is disabled because the exact focused field cannot be verified without AT-SPI2."
                      : "Wayland clipboard access alone cannot inject Ctrl+V safely.",
                  !atSpi
                      ? "Install at-spi2-core and enable desktop accessibility."
                      : qtClipboard
                      ? "Use AT-SPI insertion, or a desktop that supports the RemoteDesktop portal."
                      : "Use AT-SPI insertion, or install wl-clipboard and a desktop that supports the RemoteDesktop portal."));
    } else {
        report.capabilities.push_back(makeCapability(
            Feature::clipboard_paste,
            Availability::unsupported,
            "none",
            "Clipboard paste requires a graphical session."));
    }

    if (pipeWire) {
        report.capabilities.push_back(makeCapability(
            Feature::microphone_capture,
            Availability::available,
            "PipeWire",
            "PipeWire is the preferred low-latency microphone backend."));
    } else if (pulse) {
        report.capabilities.push_back(makeCapability(
            Feature::microphone_capture,
            Availability::degraded,
            "PulseAudio",
            "PulseAudio is available as the compatibility backend.",
            "Install PipeWire for the preferred backend."));
    } else {
        report.capabilities.push_back(makeCapability(
            Feature::microphone_capture,
            Availability::unavailable,
            "none",
            "Neither PipeWire nor PulseAudio was detected.",
            "Install PipeWire (preferred) or PulseAudio."));
    }

    if (host.executableAvailable("wpctl")) {
        report.capabilities.push_back(makeCapability(
            Feature::audio_ducking,
            Availability::available,
            "WirePlumber wpctl",
            "Default output volume can be lowered and restored."));
    } else if (host.executableAvailable("pactl")) {
        report.capabilities.push_back(makeCapability(
            Feature::audio_ducking,
            Availability::degraded,
            "PulseAudio pactl",
            "Default output volume can be lowered and restored on most PulseAudio setups."));
    } else {
        report.capabilities.push_back(makeCapability(
            Feature::audio_ducking,
            Availability::unavailable,
            "none",
            "No safe session-level mixer control was found.",
            "Install WirePlumber (wpctl) or PulseAudio utilities. Dictation remains usable without ducking."));
    }

    return report;
}

const char* toString(SessionType value) noexcept {
    switch (value) {
        case SessionType::x11: return "x11";
        case SessionType::wayland: return "wayland";
        case SessionType::unknown: return "unknown";
    }
    return "unknown";
}

const char* toString(Availability value) noexcept {
    switch (value) {
        case Availability::available: return "available";
        case Availability::permission_required: return "permission-required";
        case Availability::degraded: return "degraded";
        case Availability::unavailable: return "unavailable";
        case Availability::unsupported: return "unsupported";
    }
    return "unavailable";
}

const char* toString(Feature value) noexcept {
    switch (value) {
        case Feature::global_shortcut: return "global-shortcut";
        case Feature::active_application: return "active-application";
        case Feature::screen_capture: return "screen-capture";
        case Feature::accessibility_context: return "accessibility-context";
        case Feature::accessibility_insertion: return "accessibility-insertion";
        case Feature::clipboard_paste: return "clipboard-paste";
        case Feature::microphone_capture: return "microphone-capture";
        case Feature::audio_ducking: return "audio-ducking";
    }
    return "unknown";
}

}  // namespace localflow::platform::linux
