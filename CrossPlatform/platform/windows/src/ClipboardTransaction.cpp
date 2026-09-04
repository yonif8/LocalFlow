#include "localflow/windows/ClipboardTransaction.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <Ole2.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace localflow::windows {
namespace {

bool open_clipboard_with_retry(std::error_code& error) noexcept {
    constexpr int kAttempts = 8;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (OpenClipboard(nullptr)) {
            error.clear();
            return true;
        }
        error = last_win32_error();
        std::this_thread::sleep_for(std::chrono::milliseconds(8 * (attempt + 1)));
    }
    return false;
}

}  // namespace

std::unique_ptr<ClipboardTransaction> ClipboardTransaction::begin(
    const std::wstring_view transient_text, std::error_code& error) {
    auto transaction = std::unique_ptr<ClipboardTransaction>(new ClipboardTransaction());
    if (!transaction->initialize(transient_text, error)) {
        return nullptr;
    }
    return transaction;
}

ClipboardTransaction::~ClipboardTransaction() {
    std::error_code ignored;
    (void)restore_if_unchanged(ignored);
    release_resources();
}

bool ClipboardTransaction::initialize(const std::wstring_view text, std::error_code& error) {
    error.clear();
    if (text.empty()) {
        error = win32_error(ERROR_INVALID_PARAMETER);
        return false;
    }

    owner_thread_id_ = GetCurrentThreadId();
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) {
        error = hresult_error(initialized);
        return false;
    }
    ole_initialized_ = true;

    const HRESULT snapshot = OleGetClipboard(&previous_);
    if (FAILED(snapshot)) {
        // An actually empty clipboard has no IDataObject to retain. Any other
        // failure would make preservation unreliable, so fail before writing.
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)
            || IsClipboardFormatAvailable(CF_TEXT)
            || CountClipboardFormats() != 0) {
            error = hresult_error(snapshot);
            release_resources();
            return false;
        }
        prior_clipboard_was_empty_ = true;
    }

    if (!set_unicode_text(text, error)) {
        // EmptyClipboard may have succeeded before allocation/ownership failed.
        // Restore unconditionally here because nobody could have observed a
        // successful transient transaction from this API.
        if (previous_ != nullptr) {
            (void)OleSetClipboard(previous_);
        } else if (prior_clipboard_was_empty_) {
            std::error_code ignored;
            (void)clear_clipboard(ignored);
        }
        release_resources();
        return false;
    }

    transient_sequence_number_ = GetClipboardSequenceNumber();
    active_ = true;
    return true;
}

bool ClipboardTransaction::set_unicode_text(
    const std::wstring_view text, std::error_code& error) {
    if (!open_clipboard_with_retry(error)) {
        return false;
    }

    if (!EmptyClipboard()) {
        error = last_win32_error();
        CloseClipboard();
        return false;
    }

    const SIZE_T bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (storage == nullptr) {
        error = last_win32_error();
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(storage);
    if (destination == nullptr) {
        error = last_win32_error();
        GlobalFree(storage);
        CloseClipboard();
        return false;
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(destination)[text.size()] = L'\0';
    GlobalUnlock(storage);

    if (SetClipboardData(CF_UNICODETEXT, storage) == nullptr) {
        error = last_win32_error();
        GlobalFree(storage);
        CloseClipboard();
        return false;
    }
    // Clipboard owns storage after successful SetClipboardData.
    CloseClipboard();
    error.clear();
    return true;
}

ClipboardRestoreResult ClipboardTransaction::restore_if_unchanged(
    std::error_code& error) noexcept {
    error.clear();
    if (!active_) {
        return ClipboardRestoreResult::already_restored;
    }
    if (GetCurrentThreadId() != owner_thread_id_) {
        error = win32_error(ERROR_INVALID_THREAD_ID);
        return ClipboardRestoreResult::failed;
    }

    const std::uint32_t current_sequence = GetClipboardSequenceNumber();
    if (current_sequence != transient_sequence_number_) {
        active_ = false;
        return ClipboardRestoreResult::skipped_because_user_changed_clipboard;
    }

    HRESULT restored = S_OK;
    if (previous_ != nullptr) {
        restored = OleSetClipboard(previous_);
    } else {
        if (!clear_clipboard(error)) {
            return ClipboardRestoreResult::failed;
        }
    }
    if (FAILED(restored)) {
        error = hresult_error(restored);
        return ClipboardRestoreResult::failed;
    }
    active_ = false;
    return ClipboardRestoreResult::restored;
}

bool ClipboardTransaction::clear_clipboard(std::error_code& error) noexcept {
    if (!open_clipboard_with_retry(error)) {
        return false;
    }
    if (!EmptyClipboard()) {
        error = last_win32_error();
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    error.clear();
    return true;
}

void ClipboardTransaction::release_resources() noexcept {
    if (previous_ != nullptr) {
        previous_->Release();
        previous_ = nullptr;
    }
    if (ole_initialized_) {
        OleUninitialize();
        ole_initialized_ = false;
    }
}

}  // namespace localflow::windows

#endif
