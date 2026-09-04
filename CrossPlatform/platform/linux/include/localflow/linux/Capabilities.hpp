#pragma once

#include <optional>
#include <string>
#include <vector>

namespace localflow::platform::linux {

enum class SessionType {
    x11,
    wayland,
    unknown,
};

enum class Availability {
    available,
    permission_required,
    degraded,
    unavailable,
    unsupported,
};

enum class Feature {
    global_shortcut,
    active_application,
    screen_capture,
    accessibility_context,
    accessibility_insertion,
    clipboard_paste,
    microphone_capture,
    audio_ducking,
};

struct DesktopSession {
    SessionType type{SessionType::unknown};
    std::string desktop;
    std::string display;
    std::string waylandDisplay;
    bool sandboxed{false};
};

struct Capability {
    Feature feature;
    Availability availability{Availability::unavailable};
    std::string backend;
    std::string detail;
    std::string remediation;

    [[nodiscard]] bool usable() const noexcept {
        return availability == Availability::available ||
               availability == Availability::permission_required ||
               availability == Availability::degraded;
    }
};

struct CapabilityReport {
    DesktopSession session;
    std::vector<Capability> capabilities;

    [[nodiscard]] const Capability* find(Feature feature) const noexcept;
    [[nodiscard]] bool canShipCoreDictation() const noexcept;
};

class HostProbe {
public:
    virtual ~HostProbe() = default;

    [[nodiscard]] virtual std::optional<std::string> environment(
        const std::string& name) const = 0;
    [[nodiscard]] virtual bool executableAvailable(
        const std::string& name) const = 0;
    [[nodiscard]] virtual bool sharedLibraryAvailable(
        const std::vector<std::string>& sonames) const = 0;
    [[nodiscard]] virtual bool sessionBusNameAvailable(
        const std::string& busName) const = 0;
    [[nodiscard]] virtual bool portalInterfaceAvailable(
        const std::string& interfaceName) const = 0;
};

class SystemHostProbe final : public HostProbe {
public:
    [[nodiscard]] std::optional<std::string> environment(
        const std::string& name) const override;
    [[nodiscard]] bool executableAvailable(
        const std::string& name) const override;
    [[nodiscard]] bool sharedLibraryAvailable(
        const std::vector<std::string>& sonames) const override;
    [[nodiscard]] bool sessionBusNameAvailable(
        const std::string& busName) const override;
    [[nodiscard]] bool portalInterfaceAvailable(
        const std::string& interfaceName) const override;
};

class CapabilityDetector {
public:
    [[nodiscard]] CapabilityReport detect(const HostProbe& host) const;
};

[[nodiscard]] const char* toString(SessionType value) noexcept;
[[nodiscard]] const char* toString(Availability value) noexcept;
[[nodiscard]] const char* toString(Feature value) noexcept;

}  // namespace localflow::platform::linux
