#include "localflow/windows/TextInserter.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace localflow::windows {
namespace {

constexpr ULONG_PTR kLocalFlowInputMarker = static_cast<ULONG_PTR>(0x4C464C4F57ULL);

std::error_code incomplete_send_input() noexcept {
    const DWORD last_error = GetLastError();
    // UIPI commonly reports a zero return without setting last-error when the
    // foreground app is elevated above LocalFlow's integrity level.
    return win32_error(last_error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : last_error);
}

}  // namespace

ForegroundTextInserter::ForegroundTextInserter(TextInsertionOptions options)
    : options_(options) {}

std::error_code ForegroundTextInserter::insert(
    const std::wstring_view text,
    const std::optional<ForegroundWindowIdentity>& expected_target,
    TextInsertionOutcome* outcome) const {
    if (text.empty()) {
        return win32_error(ERROR_INVALID_PARAMETER);
    }
    if (expected_target.has_value() && !is_still_foreground(*expected_target)) {
        return win32_error(ERROR_CANCELLED);
    }

    std::error_code paste_error;
    if (options_.try_clipboard_paste) {
        paste_error = paste(text, expected_target, outcome);
        if (!paste_error) {
            return {};
        }
    }
    if (!options_.allow_unicode_fallback) {
        return paste_error ? paste_error : win32_error(ERROR_NOT_SUPPORTED);
    }
    if (expected_target.has_value() && !is_still_foreground(*expected_target)) {
        return win32_error(ERROR_CANCELLED);
    }
    auto error = type_unicode(text);
    if (!error && outcome != nullptr) {
        outcome->strategy = InsertionStrategy::unicode_input;
        outcome->clipboard_restore = ClipboardRestoreResult::already_restored;
        outcome->clipboard_restore_error.clear();
    }
    return error;
}

std::error_code ForegroundTextInserter::insert_utf8(
    const std::string_view text,
    const std::optional<ForegroundWindowIdentity>& expected_target,
    TextInsertionOutcome* outcome) const {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return win32_error(ERROR_INVALID_PARAMETER);
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return last_win32_error();
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            wide.data(),
            required)
        != required) {
        return last_win32_error();
    }
    return insert(wide, expected_target, outcome);
}

std::error_code ForegroundTextInserter::paste(
    const std::wstring_view text,
    const std::optional<ForegroundWindowIdentity>& expected_target,
    TextInsertionOutcome* outcome) const {
    std::error_code operation_error;
    std::error_code restore_error;
    ClipboardRestoreResult restore = ClipboardRestoreResult::already_restored;

    // OLE clipboard preservation requires an STA. A private short-lived thread
    // makes paste reliable regardless of Qt/the caller's COM apartment.
    try {
        std::thread clipboard_thread([&] {
            std::error_code error;
            auto clipboard = ClipboardTransaction::begin(text, error);
            if (!clipboard) {
                operation_error = error;
                return;
            }
            if (expected_target.has_value() && !is_still_foreground(*expected_target)) {
                std::error_code ignored;
                (void)clipboard->restore_if_unchanged(ignored);
                operation_error = win32_error(ERROR_CANCELLED);
                return;
            }
            operation_error = send_ctrl_v();
            if (operation_error) {
                std::error_code ignored;
                (void)clipboard->restore_if_unchanged(ignored);
                return;
            }
            if (options_.clipboard_restore_delay.count() > 0) {
                std::this_thread::sleep_for(options_.clipboard_restore_delay);
            }
            restore = clipboard->restore_if_unchanged(restore_error);
        });
        clipboard_thread.join();
    } catch (const std::system_error& error) {
        return error.code();
    }

    if (operation_error) {
        return operation_error;
    }
    if (outcome != nullptr) {
        outcome->strategy = InsertionStrategy::clipboard_paste;
        outcome->clipboard_restore = restore;
        outcome->clipboard_restore_error = restore_error;
    }
    // Ctrl+V has already been delivered. Restoration trouble is diagnostic,
    // not an insertion failure: falling back now would duplicate the text.
    return {};
}

std::error_code ForegroundTextInserter::send_ctrl_v() {
    const bool control_already_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    INPUT events[4]{};
    UINT count = 0;
    if (!control_already_down) {
        events[count].type = INPUT_KEYBOARD;
        events[count].ki.wVk = VK_CONTROL;
        events[count].ki.dwExtraInfo = kLocalFlowInputMarker;
        ++count;
    }
    events[count].type = INPUT_KEYBOARD;
    events[count].ki.wVk = 'V';
    events[count].ki.dwExtraInfo = kLocalFlowInputMarker;
    ++count;
    events[count].type = INPUT_KEYBOARD;
    events[count].ki.wVk = 'V';
    events[count].ki.dwFlags = KEYEVENTF_KEYUP;
    events[count].ki.dwExtraInfo = kLocalFlowInputMarker;
    ++count;
    if (!control_already_down) {
        events[count].type = INPUT_KEYBOARD;
        events[count].ki.wVk = VK_CONTROL;
        events[count].ki.dwFlags = KEYEVENTF_KEYUP;
        events[count].ki.dwExtraInfo = kLocalFlowInputMarker;
        ++count;
    }

    SetLastError(ERROR_SUCCESS);
    return SendInput(count, events, sizeof(INPUT)) == count ? std::error_code{}
                                                            : incomplete_send_input();
}

std::error_code ForegroundTextInserter::type_unicode(const std::wstring_view text) {
    // Keep each SendInput call bounded; enormous input arrays have shown poor
    // behavior in RDP and older Win32 controls.
    constexpr std::size_t kCodeUnitsPerBatch = 64;
    std::vector<INPUT> events;
    events.reserve(kCodeUnitsPerBatch * 2U);

    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t length = std::min(kCodeUnitsPerBatch, text.size() - offset);
        events.clear();
        for (std::size_t index = 0; index < length; ++index) {
            const WORD unit = static_cast<WORD>(text[offset + index]);
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = unit;
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            down.ki.dwExtraInfo = kLocalFlowInputMarker;
            events.push_back(down);

            INPUT up = down;
            up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            events.push_back(up);
        }
        SetLastError(ERROR_SUCCESS);
        const UINT expected = static_cast<UINT>(events.size());
        if (SendInput(expected, events.data(), sizeof(INPUT)) != expected) {
            return incomplete_send_input();
        }
        offset += length;
    }
    return {};
}

}  // namespace localflow::windows

#endif
