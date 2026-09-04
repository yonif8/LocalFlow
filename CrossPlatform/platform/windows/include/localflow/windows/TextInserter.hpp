#pragma once

#ifdef _WIN32

#include "localflow/windows/ClipboardTransaction.hpp"
#include "localflow/windows/FocusedTextTarget.hpp"
#include "localflow/windows/ForegroundWindow.hpp"

#include <chrono>
#include <optional>
#include <string_view>
#include <system_error>

namespace localflow::windows {

enum class InsertionStrategy {
    clipboard_paste,
    unicode_input,
};

struct TextInsertionOptions {
    bool try_clipboard_paste{true};
    bool allow_unicode_fallback{true};
    std::chrono::milliseconds clipboard_restore_delay{350};
};

struct TextInsertionOutcome {
    InsertionStrategy strategy{InsertionStrategy::clipboard_paste};
    ClipboardRestoreResult clipboard_restore{ClipboardRestoreResult::already_restored};
    /// A successful paste is never repeated just because restoration failed.
    /// This field lets diagnostics warn the user without duplicating text.
    std::error_code clipboard_restore_error;
    /// Exact-field validation result. PlatformBridge should retain the final
    /// transcript for copy/retry when this is anything other than `ready`.
    FocusedTextTargetStatus target_status{FocusedTextTargetStatus::ready};
    std::error_code target_error;
};

/// Synchronous Win32 inserter intended to run on the pipeline worker, never the
/// UI thread. Clipboard + Ctrl+V is preferred for Electron, browsers, Office,
/// and terminals; KEYEVENTF_UNICODE is the no-clipboard fallback.
class ForegroundTextInserter final {
public:
    explicit ForegroundTextInserter(TextInsertionOptions options = {});

    [[nodiscard]] std::error_code insert(
        std::wstring_view text,
        const std::optional<ForegroundWindowIdentity>& expected_target,
        TextInsertionOutcome* outcome = nullptr) const;

    [[nodiscard]] std::error_code insert_utf8(
        std::string_view text,
        const std::optional<ForegroundWindowIdentity>& expected_target,
        TextInsertionOutcome* outcome = nullptr) const;

    /// Fail-closed insertion path for dictation. The exact UIA/native field
    /// captured at PTT press is revalidated immediately before each synthetic
    /// input operation. There is deliberately no window-only fallback.
    [[nodiscard]] std::error_code insert_into_focused_target(
        std::wstring_view text,
        const FocusedTextTargetIdentity& expected_target,
        TextInsertionOutcome* outcome = nullptr) const;

    [[nodiscard]] std::error_code insert_utf8_into_focused_target(
        std::string_view text,
        const FocusedTextTargetIdentity& expected_target,
        TextInsertionOutcome* outcome = nullptr) const;

private:
    [[nodiscard]] std::error_code insert_impl(
        std::wstring_view text,
        const std::optional<ForegroundWindowIdentity>& expected_window,
        const FocusedTextTargetIdentity* expected_field,
        TextInsertionOutcome* outcome) const;
    [[nodiscard]] std::error_code paste(
        std::wstring_view text,
        const std::optional<ForegroundWindowIdentity>& expected_window,
        const FocusedTextTargetIdentity* expected_field,
        TextInsertionOutcome* outcome) const;
    [[nodiscard]] static std::error_code type_unicode(
        std::wstring_view text,
        const std::optional<ForegroundWindowIdentity>& expected_window,
        const FocusedTextTargetIdentity* expected_field,
        TextInsertionOutcome* outcome);
    [[nodiscard]] static std::error_code send_ctrl_v();

    TextInsertionOptions options_;
};

}  // namespace localflow::windows

#endif
