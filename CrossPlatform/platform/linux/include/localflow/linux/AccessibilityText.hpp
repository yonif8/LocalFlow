#pragma once

#include "localflow/linux/ScreenContext.hpp"
#include "localflow/platform/AccessibilityText.hpp"

namespace localflow::platform::linux {

/// Captures visible accessibility names in the window that owned the exact
/// press-time AT-SPI target. The target must still be focused, editable, and
/// proven non-secure. Password/unknown-security subtrees are skipped before
/// their names are read. Failure is represented by an empty snapshot because
/// accessibility context is opportunistic and must never block dictation.
[[nodiscard]] localflow::platform::AccessibilityTextSnapshot
captureVisibleAccessibilityText(
    const ApplicationInfo& expected,
    localflow::platform::AccessibilityTextLimits limits = {}) noexcept;

}  // namespace localflow::platform::linux
