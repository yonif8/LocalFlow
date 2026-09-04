#include "localflow/windows/WinError.hpp"

#ifdef _WIN32

#include <string>

namespace localflow::windows {
namespace {

class HResultCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "HRESULT"; }

    [[nodiscard]] std::string message(const int condition) const override {
        char* buffer = nullptr;
        const auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD count = FormatMessageA(
            flags,
            nullptr,
            static_cast<DWORD>(condition),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&buffer),
            0,
            nullptr);
        if (count == 0 || buffer == nullptr) {
            return "HRESULT 0x" + std::to_string(static_cast<unsigned int>(condition));
        }
        std::string result(buffer, count);
        LocalFree(buffer);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
        return result;
    }
};

const HResultCategory kHResultCategory;

}  // namespace

std::error_code last_win32_error() noexcept {
    return win32_error(GetLastError());
}

std::error_code win32_error(const DWORD value) noexcept {
    return {static_cast<int>(value), std::system_category()};
}

std::error_code hresult_error(const HRESULT value) noexcept {
    return {static_cast<int>(value), kHResultCategory};
}

}  // namespace localflow::windows

#endif
