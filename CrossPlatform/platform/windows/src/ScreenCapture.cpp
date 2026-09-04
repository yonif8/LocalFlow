#include "localflow/windows/ScreenCapture.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace localflow::windows {
namespace {

constexpr LONG kMaximumCaptureDimension = 16'384;
constexpr std::uint64_t kMaximumCapturePixels = 64ULL * 1024ULL * 1024ULL;

class ScreenDc final {
public:
    explicit ScreenDc(const HWND window) : window_(window), value_(GetDC(window)) {}
    ~ScreenDc() {
        if (value_ != nullptr) {
            ReleaseDC(window_, value_);
        }
    }
    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HWND window_;
    HDC value_;
};

class MemoryDc final {
public:
    explicit MemoryDc(const HDC source) : value_(CreateCompatibleDC(source)) {}
    ~MemoryDc() {
        if (value_ != nullptr) {
            DeleteDC(value_);
        }
    }
    [[nodiscard]] HDC get() const noexcept { return value_; }

private:
    HDC value_;
};

class BitmapSelection final {
public:
    BitmapSelection(const HDC dc, const HBITMAP bitmap)
        : dc_(dc), previous_(SelectObject(dc, bitmap)) {}
    ~BitmapSelection() {
        if (dc_ != nullptr && previous_ != nullptr && previous_ != HGDI_ERROR) {
            SelectObject(dc_, previous_);
        }
    }
    [[nodiscard]] bool valid() const noexcept {
        return previous_ != nullptr && previous_ != HGDI_ERROR;
    }

private:
    HDC dc_;
    HGDIOBJ previous_;
};

}  // namespace

GdiWindowCapture::GdiWindowCapture(GdiCaptureOptions options) : options_(options) {}

std::optional<ScreenFrame> GdiWindowCapture::capture(
    const ForegroundWindowIdentity& target, std::error_code& error) {
    error.clear();
    const HWND window = target.handle;
    if (window == nullptr || !IsWindow(window) || IsIconic(window)) {
        error = win32_error(ERROR_INVALID_WINDOW_HANDLE);
        return std::nullopt;
    }

    DWORD affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &affinity)
        && (affinity == WDA_EXCLUDEFROMCAPTURE || affinity == WDA_MONITOR)) {
        error = win32_error(ERROR_ACCESS_DENIED);
        return std::nullopt;
    }

    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) {
        error = last_win32_error();
        return std::nullopt;
    }
    const LONG raw_width = bounds.right - bounds.left;
    const LONG raw_height = bounds.bottom - bounds.top;
    if (raw_width <= 0 || raw_height <= 0 ||
        raw_width > kMaximumCaptureDimension ||
        raw_height > kMaximumCaptureDimension ||
        static_cast<std::uint64_t>(raw_width) *
                static_cast<std::uint64_t>(raw_height) >
            kMaximumCapturePixels) {
        error = win32_error(ERROR_INVALID_DATA);
        return std::nullopt;
    }
    const auto width = static_cast<std::uint32_t>(raw_width);
    const auto height = static_cast<std::uint32_t>(raw_height);
    const std::uint64_t byte_count = static_cast<std::uint64_t>(width) * height * 4U;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }

    ScreenDc screen(nullptr);
    if (screen.get() == nullptr) {
        error = last_win32_error();
        return std::nullopt;
    }
    MemoryDc memory(screen.get());
    if (memory.get() == nullptr) {
        error = last_win32_error();
        return std::nullopt;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = raw_width;
    bitmap_info.bmiHeader.biHeight = -raw_height;  // top-down
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(
        screen.get(), &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        error = last_win32_error();
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        return std::nullopt;
    }

    bool captured = false;
    {
        BitmapSelection selected(memory.get(), bitmap);
        if (!selected.valid()) {
            error = last_win32_error();
            DeleteObject(bitmap);
            return std::nullopt;
        }
        if (options_.try_print_window_first) {
            captured = PrintWindow(window, memory.get(), PW_RENDERFULLCONTENT) != FALSE;
        }
        if (!captured && options_.allow_visible_pixels_fallback) {
            captured = BitBlt(
                           memory.get(),
                           0,
                           0,
                           raw_width,
                           raw_height,
                           screen.get(),
                           bounds.left,
                           bounds.top,
                           SRCCOPY | CAPTUREBLT)
                != FALSE;
        }
        if (captured) {
            GdiFlush();
        }
    }

    if (!captured) {
        error = last_win32_error();
        if (!error) {
            error = win32_error(ERROR_NOT_SUPPORTED);
        }
        DeleteObject(bitmap);
        return std::nullopt;
    }

    ScreenFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = width * 4U;
    frame.source_window = window;
    const auto* first = static_cast<const std::uint8_t*>(pixels);
    try {
        frame.bgra.assign(first, first + static_cast<std::size_t>(byte_count));
    } catch (const std::bad_alloc&) {
        DeleteObject(bitmap);
        error = win32_error(ERROR_NOT_ENOUGH_MEMORY);
        return std::nullopt;
    }
    DeleteObject(bitmap);

    // BI_RGB does not guarantee a useful alpha channel. OCR consumers expect
    // an opaque BGRA image, so normalize it without touching color data.
    for (std::size_t index = 3; index < frame.bgra.size(); index += 4) {
        frame.bgra[index] = 0xFF;
    }
    return frame;
}

}  // namespace localflow::windows

#endif
