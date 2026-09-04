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

// Event-loop-neutral boundary for org.freedesktop.portal.GlobalShortcuts.
// A Qt/QDBus or GDBus implementation owns the portal session and maps its
// Activated/Deactivated signals to this callback. This keeps portal consent
// UI on the UI thread instead of hiding it in the dictation core.
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
