#pragma once

#include "localflow/platform/AccessibilityText.hpp"
#include "localflow/windows/FocusedTextTarget.hpp"

#ifdef _WIN32

namespace localflow::windows {

/// Reads visible accessibility names from the press-time foreground window.
/// The exact editable target is revalidated first, password subtrees are never
/// read, and the traversal obeys every supplied limit. Failures return an
/// empty snapshot because this context is always opportunistic.
[[nodiscard]] localflow::platform::AccessibilityTextSnapshot
capture_visible_accessibility_text(
    const FocusedTextTargetIdentity& expected,
    localflow::platform::AccessibilityTextLimits limits = {}) noexcept;

}  // namespace localflow::windows

#endif
