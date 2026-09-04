#pragma once

#ifdef _WIN32

#include "localflow/windows/ForegroundWindow.hpp"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace localflow::windows {

/// Top-down, tightly packed BGRA8 frame suitable for Windows.Media.Ocr,
/// Tesseract, or another entirely local OCR backend.
struct ScreenFrame {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride_bytes{0};
    HWND source_window{nullptr};
    std::vector<std::uint8_t> bgra;
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    [[nodiscard]] virtual std::optional<ScreenFrame> capture(
        const ForegroundWindowIdentity& target, std::error_code& error) = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
};

struct GdiCaptureOptions {
    /// PrintWindow avoids LocalFlow's HUD and other overlapping windows. Some
    /// GPU-rendered apps decline it, in which case BitBlt is used automatically.
    bool try_print_window_first{true};
    bool allow_visible_pixels_fallback{true};
};

/// Dependency-free baseline capture backend. It is useful for Win32 coverage
/// and tests, while production can inject a Windows.Graphics.Capture backend
/// through IScreenCapture without changing the terminology pipeline.
class GdiWindowCapture final : public IScreenCapture {
public:
    explicit GdiWindowCapture(GdiCaptureOptions options = {});

    [[nodiscard]] std::optional<ScreenFrame> capture(
        const ForegroundWindowIdentity& target, std::error_code& error) override;
    [[nodiscard]] std::string_view backend_name() const noexcept override { return "gdi"; }

private:
    GdiCaptureOptions options_;
};

}  // namespace localflow::windows

#endif
