#include "localflow/windows/ScreenCapture.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace localflow::windows {
namespace {

constexpr LONG kMaximumCaptureDimension = 16'384;
constexpr std::uint64_t kMaximumCapturePixels = 64ULL * 1024ULL * 1024ULL;
// Produce a frame that already fits WindowsMediaOcr's default working budget,
// avoiding a second resize for common high-DPI and ultrawide windows. The OCR
// adapter still applies the runtime's (potentially smaller) dimension limit.
constexpr LONG kMaximumOutputDimension = 2'400;
constexpr std::uint64_t kMaximumOutputPixels = 6ULL * 1024ULL * 1024ULL;

struct CaptureDimensions {
    LONG width{0};
    LONG height{0};
    bool downscaled{false};
};

[[nodiscard]] bool checkedProduct(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
    if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] DWORD lastErrorOr(const DWORD fallback) noexcept {
    const DWORD code = GetLastError();
    return code == ERROR_SUCCESS ? fallback : code;
}

[[nodiscard]] std::optional<CaptureDimensions> outputDimensions(
    const LONG source_width,
    const LONG source_height,
    const std::uint64_t source_pixels,
    std::error_code& error) {
    CaptureDimensions output{source_width, source_height, false};
    const LONG source_maximum = std::max(source_width, source_height);
    if (source_maximum <= kMaximumOutputDimension &&
        source_pixels <= kMaximumOutputPixels) {
        return output;
    }

    const double dimension_scale =
        static_cast<double>(kMaximumOutputDimension) / source_maximum;
    const double pixel_scale = std::sqrt(
        static_cast<double>(kMaximumOutputPixels) /
        static_cast<double>(source_pixels));
    const double scale = std::min(dimension_scale, pixel_scale);
    if (!std::isfinite(scale) || scale <= 0.0 || scale >= 1.0) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }
    output.width = std::max<LONG>(
        1, static_cast<LONG>(std::floor(static_cast<double>(source_width) * scale)));
    output.height = std::max<LONG>(
        1, static_cast<LONG>(std::floor(static_cast<double>(source_height) * scale)));
    output.downscaled = true;

    std::uint64_t output_pixels = 0;
    if (!checkedProduct(
            static_cast<std::uint64_t>(output.width),
            static_cast<std::uint64_t>(output.height), output_pixels)) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }
    // Floating-point rounding must never move either output dimension or the
    // allocation above its hard budget. This loop executes at most a handful
    // of times at these bounds.
    while (output.width > kMaximumOutputDimension ||
           output.height > kMaximumOutputDimension ||
           output_pixels > kMaximumOutputPixels) {
        if (output.width >= output.height && output.width > 1) {
            --output.width;
        } else if (output.height > 1) {
            --output.height;
        } else {
            error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
            return std::nullopt;
        }
        output_pixels = static_cast<std::uint64_t>(output.width) *
            static_cast<std::uint64_t>(output.height);
    }
    return output;
}

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
        error = win32_error(lastErrorOr(ERROR_INVALID_WINDOW_HANDLE));
        return std::nullopt;
    }
    const std::int64_t raw_width_64 =
        static_cast<std::int64_t>(bounds.right) - bounds.left;
    const std::int64_t raw_height_64 =
        static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    if (raw_width_64 <= 0 || raw_height_64 <= 0) {
        error = win32_error(ERROR_INVALID_DATA);
        return std::nullopt;
    }
    if (raw_width_64 > kMaximumCaptureDimension ||
        raw_height_64 > kMaximumCaptureDimension) {
        error = win32_error(ERROR_FILE_TOO_LARGE);
        return std::nullopt;
    }
    const LONG raw_width = static_cast<LONG>(raw_width_64);
    const LONG raw_height = static_cast<LONG>(raw_height_64);
    std::uint64_t source_pixels = 0;
    if (!checkedProduct(
            static_cast<std::uint64_t>(raw_width),
            static_cast<std::uint64_t>(raw_height), source_pixels)) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }
    if (source_pixels > kMaximumCapturePixels) {
        error = win32_error(ERROR_FILE_TOO_LARGE);
        return std::nullopt;
    }

    const auto dimensions = outputDimensions(
        raw_width, raw_height, source_pixels, error);
    if (!dimensions.has_value()) return std::nullopt;
    if (dimensions->downscaled && !options_.allow_visible_pixels_fallback) {
        // PrintWindow renders at the source window's native size; asking it to
        // draw into a smaller DIB clips instead of scaling. A visible-pixel
        // StretchBlt is therefore the only bounded downscaled path.
        error = win32_error(ERROR_NOT_SUPPORTED);
        return std::nullopt;
    }
    const auto width = static_cast<std::uint32_t>(dimensions->width);
    const auto height = static_cast<std::uint32_t>(dimensions->height);
    if (width > std::numeric_limits<std::uint32_t>::max() / 4U) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }
    std::uint64_t output_pixels = 0;
    std::uint64_t byte_count = 0;
    if (!checkedProduct(width, height, output_pixels) ||
        !checkedProduct(output_pixels, 4U, byte_count) ||
        output_pixels > kMaximumOutputPixels ||
        byte_count > std::numeric_limits<std::size_t>::max()) {
        error = win32_error(ERROR_ARITHMETIC_OVERFLOW);
        return std::nullopt;
    }

    SetLastError(ERROR_SUCCESS);
    ScreenDc screen(nullptr);
    if (screen.get() == nullptr) {
        error = win32_error(lastErrorOr(ERROR_NOT_ENOUGH_MEMORY));
        return std::nullopt;
    }
    SetLastError(ERROR_SUCCESS);
    MemoryDc memory(screen.get());
    if (memory.get() == nullptr) {
        error = win32_error(lastErrorOr(ERROR_NOT_ENOUGH_MEMORY));
        return std::nullopt;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = dimensions->width;
    bitmap_info.bmiHeader.biHeight = -dimensions->height;  // top-down
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    SetLastError(ERROR_SUCCESS);
    const HBITMAP bitmap = CreateDIBSection(
        screen.get(), &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        error = win32_error(lastErrorOr(ERROR_NOT_ENOUGH_MEMORY));
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        return std::nullopt;
    }

    bool captured = false;
    DWORD capture_error = ERROR_SUCCESS;
    {
        SetLastError(ERROR_SUCCESS);
        BitmapSelection selected(memory.get(), bitmap);
        if (!selected.valid()) {
            error = win32_error(lastErrorOr(ERROR_GEN_FAILURE));
            DeleteObject(bitmap);
            return std::nullopt;
        }
        if (!dimensions->downscaled && options_.try_print_window_first) {
            SetLastError(ERROR_SUCCESS);
            captured = PrintWindow(window, memory.get(), PW_RENDERFULLCONTENT) != FALSE;
            if (!captured) {
                capture_error = lastErrorOr(ERROR_NOT_SUPPORTED);
            }
        }
        if (!captured && options_.allow_visible_pixels_fallback) {
            capture_error = ERROR_SUCCESS;
            if (dimensions->downscaled) {
                SetLastError(ERROR_SUCCESS);
                const int previous_mode = SetStretchBltMode(memory.get(), HALFTONE);
                if (previous_mode == 0) {
                    capture_error = lastErrorOr(ERROR_GEN_FAILURE);
                } else {
                    SetLastError(ERROR_SUCCESS);
                    if (!SetBrushOrgEx(memory.get(), 0, 0, nullptr)) {
                        capture_error = lastErrorOr(ERROR_GEN_FAILURE);
                    } else {
                        SetLastError(ERROR_SUCCESS);
                        captured = StretchBlt(
                                       memory.get(),
                                       0,
                                       0,
                                       dimensions->width,
                                       dimensions->height,
                                       screen.get(),
                                       bounds.left,
                                       bounds.top,
                                       raw_width,
                                       raw_height,
                                       SRCCOPY | CAPTUREBLT)
                            != FALSE;
                        if (!captured) {
                            capture_error = lastErrorOr(ERROR_GEN_FAILURE);
                        }
                    }
                }
            } else {
                SetLastError(ERROR_SUCCESS);
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
                if (!captured) {
                    capture_error = lastErrorOr(ERROR_GEN_FAILURE);
                }
            }
        }
        if (captured) {
            SetLastError(ERROR_SUCCESS);
            if (!GdiFlush()) {
                captured = false;
                capture_error = lastErrorOr(ERROR_GEN_FAILURE);
            }
        }
    }

    if (!captured) {
        error = win32_error(
            capture_error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : capture_error);
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
