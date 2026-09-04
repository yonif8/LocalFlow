#pragma once

#ifdef _WIN32

#include <Windows.h>

#include <system_error>

namespace localflow::windows {

[[nodiscard]] std::error_code last_win32_error() noexcept;
[[nodiscard]] std::error_code win32_error(DWORD value) noexcept;
[[nodiscard]] std::error_code hresult_error(HRESULT value) noexcept;

}  // namespace localflow::windows

#endif
