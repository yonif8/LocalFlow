#pragma once

#ifdef _WIN32

#include <Windows.h>
#include <objidl.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

namespace localflow::windows {

enum class ClipboardRestoreResult {
    restored,
    skipped_because_user_changed_clipboard,
    already_restored,
    failed,
};

/// Owns one transient clipboard write while retaining the original IDataObject.
/// This preserves rich formats and delayed-rendered content instead of reducing
/// the user's clipboard to plain text. Restoration is guarded by the clipboard
/// sequence number, so a copy performed during dictation is never overwritten.
///
/// The transaction is thread-affine and creates an OLE STA for its lifetime.
class ClipboardTransaction final {
public:
    [[nodiscard]] static std::unique_ptr<ClipboardTransaction> begin(
        std::wstring_view transient_text, std::error_code& error);

    ~ClipboardTransaction();

    ClipboardTransaction(const ClipboardTransaction&) = delete;
    ClipboardTransaction& operator=(const ClipboardTransaction&) = delete;

    [[nodiscard]] ClipboardRestoreResult restore_if_unchanged(
        std::error_code& error) noexcept;

    [[nodiscard]] std::uint32_t transient_sequence_number() const noexcept {
        return transient_sequence_number_;
    }

private:
    ClipboardTransaction() = default;

    [[nodiscard]] bool initialize(std::wstring_view text, std::error_code& error);
    [[nodiscard]] bool set_unicode_text(std::wstring_view text, std::error_code& error);
    [[nodiscard]] bool clear_clipboard(std::error_code& error) noexcept;
    void release_resources() noexcept;

    IDataObject* previous_{nullptr};
    DWORD owner_thread_id_{0};
    std::uint32_t transient_sequence_number_{0};
    bool ole_initialized_{false};
    bool prior_clipboard_was_empty_{false};
    bool active_{false};
};

}  // namespace localflow::windows

#endif
