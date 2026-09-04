#pragma once

#ifdef _WIN32

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

namespace localflow::windows {

struct ForegroundWindowIdentity {
    HWND handle{nullptr};
    std::uint32_t process_id{0};
    std::uint32_t thread_id{0};
    std::wstring title;
    std::wstring executable_path;
    std::wstring package_family_name;

    /// Stable app identifier for per-app tone/settings. Packaged apps use the
    /// package family name; classic Win32 apps use their canonical executable
    /// path. This intentionally does not use the mutable window title.
    [[nodiscard]] std::wstring application_id() const;
};

/// Returns the actual foreground target without activating LocalFlow or
/// changing focus. A null optional with a clear error means Windows currently
/// has no eligible foreground window (for example, the secure desktop).
[[nodiscard]] std::optional<ForegroundWindowIdentity> query_foreground_window(
    std::error_code& error) noexcept;

/// Checks that the user has not changed targets between PTT release and text
/// insertion. HWND reuse is guarded by comparing the process ID as well.
[[nodiscard]] bool is_still_foreground(const ForegroundWindowIdentity& identity) noexcept;

}  // namespace localflow::windows

#endif
