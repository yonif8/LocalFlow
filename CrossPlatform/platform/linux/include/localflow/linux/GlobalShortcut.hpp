#pragma once

#include "localflow/linux/Capabilities.hpp"
#include "localflow/linux/Status.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace localflow::platform::linux {

enum class ShortcutKind {
    key,
    mouse_button,
};

enum class ShortcutEdge {
    pressed,
    released,
    // Escape was pressed while a native hold-to-talk gesture was active.
    // Callers must discard the active recording instead of treating this as
    // an ordinary release.
    cancelled,
};

enum class Modifier {
    shift,
    control,
    alt,
    super,
};

struct ShortcutSpec {
    std::string id{"push-to-talk"};
    ShortcutKind kind{ShortcutKind::key};

    // X11 key name (for example "F8" or "space") and the preferred_trigger
    // passed to the portal (for example "CTRL+F8").
    std::string trigger{"F8"};
    std::vector<Modifier> modifiers;

    // X11 button number when kind == mouse_button. Wayland compositors do not
    // expose arbitrary global mouse buttons through the standard portal.
    std::uint32_t mouseButton{0};
};

struct ShortcutEvent {
    std::string id;
    ShortcutEdge edge{ShortcutEdge::pressed};
    std::uint64_t monotonicTimestampMs{0};
};

using ShortcutCallback = std::function<void(const ShortcutEvent&)>;

class GlobalShortcutBackend {
public:
    virtual ~GlobalShortcutBackend() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    virtual Status start(const ShortcutSpec& shortcut, ShortcutCallback callback) = 0;
    virtual void stop() noexcept = 0;
};

// Injectable boundary for org.freedesktop.portal.GlobalShortcuts. The Linux
// factory supplies a production Qt/QDBus transport when none is injected and
// maps Activated/Deactivated signals to this callback.
class GlobalShortcutsPortal {
public:
    virtual ~GlobalShortcutsPortal() = default;

    virtual Status bind(
        const ShortcutSpec& shortcut,
        ShortcutCallback callback) = 0;
    virtual void close() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> makeGlobalShortcutBackend(
    const CapabilityReport& report,
    std::shared_ptr<GlobalShortcutsPortal> portal = {});

}  // namespace localflow::platform::linux
