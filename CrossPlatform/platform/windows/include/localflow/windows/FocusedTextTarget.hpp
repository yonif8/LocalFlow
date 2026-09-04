#pragma once

#include "localflow/windows/FocusedTextTargetState.hpp"

#ifdef _WIN32

#include "localflow/windows/ForegroundWindow.hpp"

#include <optional>
#include <system_error>

namespace localflow::windows {

enum class FocusedTextTargetStatus : std::uint8_t {
    ready,
    no_foreground_window,
    no_focused_control,
    automation_unavailable,
    not_editable,
    protected_content,
    target_changed,
    invalid_target,
    system_error,
};

struct FocusedTextTargetIdentity {
    ForegroundWindowIdentity window;
    FocusedTextTargetFingerprint fingerprint;
};

struct FocusedTextTargetCapture {
    FocusedTextTargetStatus status{FocusedTextTargetStatus::system_error};
    std::optional<FocusedTextTargetIdentity> target;
    std::error_code error;

    [[nodiscard]] bool safe_for_insertion() const noexcept {
        return status == FocusedTextTargetStatus::ready && target.has_value();
    }
};

struct FocusedTextTargetValidation {
    FocusedTextTargetStatus status{FocusedTextTargetStatus::system_error};
    std::error_code error;

    [[nodiscard]] bool safe_for_insertion() const noexcept {
        return status == FocusedTextTargetStatus::ready;
    }
};

/// Captures the exact focused editable field without reading or retaining its
/// contents. UI Automation runtime identity is preferred. A native HWND
/// fallback is accepted only for a recognized Win32 Edit/RichEdit control
/// whose password and read-only state can be checked directly.
[[nodiscard]] FocusedTextTargetCapture capture_focused_text_target() noexcept;

/// Re-queries focus and fails closed unless the same field still owns focus.
/// PlatformBridge should preserve the transcript for copy/retry whenever this
/// does not return `ready`; it must not downgrade to window-only insertion.
[[nodiscard]] FocusedTextTargetValidation validate_focused_text_target(
    const FocusedTextTargetIdentity& expected) noexcept;

}  // namespace localflow::windows

#endif
