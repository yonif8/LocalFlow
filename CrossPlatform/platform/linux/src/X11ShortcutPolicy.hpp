#pragma once

#include <X11/Xlib.h>

namespace localflow::platform::linux::detail {

/// Returns true only for X11's legacy autorepeat representation: an adjacent
/// release/press pair for the held trigger with the same server identity.
[[nodiscard]] bool isLegacyAutoRepeatPair(
    bool triggerHeld,
    KeyCode configuredKeycode,
    const XEvent& release,
    const XEvent& next) noexcept;

}  // namespace localflow::platform::linux::detail
