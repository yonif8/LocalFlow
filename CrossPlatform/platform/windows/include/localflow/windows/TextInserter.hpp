#pragma once

#ifdef _WIN32

#include "localflow/windows/ClipboardTransaction.hpp"
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

private:
    [[nodiscard]] std::error_code paste(
        std::wstring_view text,
        const std::optional<ForegroundWindowIdentity>& expected_target,
        TextInsertionOutcome* outcome) const;
    [[nodiscard]] static std::error_code type_unicode(std::wstring_view text);
    [[nodiscard]] static std::error_code send_ctrl_v();

    TextInsertionOptions options_;
};

}  // namespace localflow::windows

#endif
