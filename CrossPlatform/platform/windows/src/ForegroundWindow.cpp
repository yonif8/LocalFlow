#include "localflow/windows/ForegroundWindow.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace localflow::windows {
namespace {

std::wstring window_title(const HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, result.data(), static_cast<int>(result.size()));
    result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return result;
}

std::wstring process_path(const HANDLE process) {
    DWORD capacity = 32'768;
    std::wstring result(capacity, L'\0');
    if (!QueryFullProcessImageNameW(process, 0, result.data(), &capacity)) {
        return {};
    }
    result.resize(capacity);
    return result;
}

std::wstring package_family(const HANDLE process) {
    UINT32 length = 0;
    const LONG first = GetPackageFamilyName(process, &length, nullptr);
    if (first == APPMODEL_ERROR_NO_PACKAGE) {
        return {};
    }
    if (first != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return {};
    }
    std::wstring result(length, L'\0');
    if (GetPackageFamilyName(process, &length, result.data()) != ERROR_SUCCESS) {
        return {};
    }
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

}  // namespace

std::wstring ForegroundWindowIdentity::application_id() const {
    if (!package_family_name.empty()) {
        return package_family_name;
    }
    std::wstring result = executable_path;
    std::transform(result.begin(), result.end(), result.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return result;
}

std::optional<ForegroundWindowIdentity> query_foreground_window(std::error_code& error) noexcept {
    error.clear();
    const HWND window = GetForegroundWindow();
    if (window == nullptr || !IsWindow(window)) {
        error = win32_error(ERROR_NOT_FOUND);
        return std::nullopt;
    }

    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    if (thread_id == 0 || process_id == 0) {
        error = last_win32_error();
        return std::nullopt;
    }

    ForegroundWindowIdentity result;
    result.handle = window;
    result.process_id = process_id;
    result.thread_id = thread_id;
    result.title = window_title(window);

    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
    if (process != nullptr) {
        result.executable_path = process_path(process);
        result.package_family_name = package_family(process);
        CloseHandle(process);
    }

    // Protected/elevated processes can deny path access. HWND + PID still form
    // a valid insertion/capture identity, so this is a partial success.
    return result;
}

bool is_still_foreground(const ForegroundWindowIdentity& identity) noexcept {
    const HWND current = GetForegroundWindow();
    if (current == nullptr || current != identity.handle || !IsWindow(current)) {
        return false;
    }
    DWORD current_process = 0;
    GetWindowThreadProcessId(current, &current_process);
    return current_process == identity.process_id;
}

}  // namespace localflow::windows

#endif
