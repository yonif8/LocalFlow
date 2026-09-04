#include "localflow/windows/WindowsMediaOcr.hpp"

#ifdef _WIN32

#include "localflow/windows/WinError.hpp"

#include <Windows.h>
#include <Unknwn.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

namespace localflow::windows {
namespace {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Globalization::Language;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Storage::Streams::Buffer;

class WinrtApartment final {
public:
    WinrtApartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~WinrtApartment() { winrt::uninit_apartment(); }
};

struct CompletionSignal {
    CompletionSignal() : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~CompletionSignal() {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }
    HANDLE event{nullptr};
};

struct PackedImage {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> pixels;
};

OcrRecognitionResult invalid_result(
    const ScreenFrame& frame,
    const DWORD code,
    const std::chrono::steady_clock::time_point started) {
    OcrRecognitionResult result;
    result.input_width = frame.width;
    result.input_height = frame.height;
    result.error = win32_error(code);
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

bool validate_frame(const ScreenFrame& frame, const OcrOptions& options, DWORD& error) {
    if (frame.width == 0 || frame.height == 0 || frame.stride_bytes < frame.width * 4ULL) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    if (frame.width > options.max_source_dimension || frame.height > options.max_source_dimension) {
        error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(frame.width) * frame.height;
    const std::uint64_t required = static_cast<std::uint64_t>(frame.stride_bytes) * frame.height;
    if (pixels > options.max_source_pixels || required > options.max_source_bytes
        || required > frame.bgra.size()) {
        error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    return true;
}

PackedImage prepare_image(
    const ScreenFrame& frame,
    const std::uint32_t maximum_dimension,
    const std::uint64_t maximum_pixels) {
    double scale = 1.0;
    const std::uint32_t source_max = std::max(frame.width, frame.height);
    if (source_max > maximum_dimension) {
        scale = static_cast<double>(maximum_dimension) / source_max;
    }
    const std::uint64_t source_pixels = static_cast<std::uint64_t>(frame.width) * frame.height;
    if (source_pixels > maximum_pixels) {
        scale = std::min(scale, std::sqrt(static_cast<double>(maximum_pixels) / source_pixels));
    }

    PackedImage output;
    output.width = std::max(1U, static_cast<std::uint32_t>(std::floor(frame.width * scale)));
    output.height = std::max(1U, static_cast<std::uint32_t>(std::floor(frame.height * scale)));
    output.pixels.resize(static_cast<std::size_t>(output.width) * output.height * 4U);

    if (output.width == frame.width && output.height == frame.height) {
        const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4U;
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(
                output.pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                frame.bgra.data() + static_cast<std::size_t>(row) * frame.stride_bytes,
                row_bytes);
        }
        return output;
    }

    // Bilinear filtering retains small glyph edges much better than nearest-
    // neighbour scaling on high-DPI IDE and terminal screenshots.
    const double x_ratio = static_cast<double>(frame.width) / output.width;
    const double y_ratio = static_cast<double>(frame.height) / output.height;
    for (std::uint32_t y = 0; y < output.height; ++y) {
        const double source_y = std::max(0.0, (y + 0.5) * y_ratio - 0.5);
        const auto y0 = static_cast<std::uint32_t>(source_y);
        const auto y1 = std::min(y0 + 1, frame.height - 1);
        const double fy = source_y - y0;
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const double source_x = std::max(0.0, (x + 0.5) * x_ratio - 0.5);
            const auto x0 = static_cast<std::uint32_t>(source_x);
            const auto x1 = std::min(x0 + 1, frame.width - 1);
            const double fx = source_x - x0;
            const auto* p00 = frame.bgra.data() + static_cast<std::size_t>(y0) * frame.stride_bytes
                + static_cast<std::size_t>(x0) * 4U;
            const auto* p10 = frame.bgra.data() + static_cast<std::size_t>(y0) * frame.stride_bytes
                + static_cast<std::size_t>(x1) * 4U;
            const auto* p01 = frame.bgra.data() + static_cast<std::size_t>(y1) * frame.stride_bytes
                + static_cast<std::size_t>(x0) * 4U;
            const auto* p11 = frame.bgra.data() + static_cast<std::size_t>(y1) * frame.stride_bytes
                + static_cast<std::size_t>(x1) * 4U;
            auto* destination = output.pixels.data()
                + (static_cast<std::size_t>(y) * output.width + x) * 4U;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const double top = p00[channel] + (p10[channel] - p00[channel]) * fx;
                const double bottom = p01[channel] + (p11[channel] - p01[channel]) * fx;
                destination[channel] = static_cast<std::uint8_t>(
                    std::clamp(top + (bottom - top) * fy, 0.0, 255.0));
            }
        }
    }
    return output;
}

OcrRecognitionResult recognize_frame(ScreenFrame frame, const OcrOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    DWORD validation_error = ERROR_SUCCESS;
    if (options.timeout.count() <= 0 || options.preferred_ocr_dimension == 0
        || options.preferred_ocr_pixels == 0 || !validate_frame(frame, options, validation_error)) {
        return invalid_result(
            frame,
            validation_error == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : validation_error,
            started);
    }

    OcrRecognitionResult result;
    result.input_width = frame.width;
    result.input_height = frame.height;
    try {
        WinrtApartment apartment;
        OcrEngine engine{nullptr};
        if (options.language_tag.has_value()) {
            Language language(winrt::to_hstring(*options.language_tag));
            if (!OcrEngine::IsLanguageSupported(language)) {
                return invalid_result(frame, ERROR_NOT_SUPPORTED, started);
            }
            engine = OcrEngine::TryCreateFromLanguage(language);
        } else {
            engine = OcrEngine::TryCreateFromUserProfileLanguages();
        }
        if (!engine) {
            return invalid_result(frame, ERROR_NOT_SUPPORTED, started);
        }
        result.recognition_language = winrt::to_string(engine.RecognizerLanguage().LanguageTag());

        const auto runtime_limit = static_cast<std::uint32_t>(OcrEngine::MaxImageDimension());
        const auto dimension_limit = std::min(options.preferred_ocr_dimension, runtime_limit);
        if (dimension_limit == 0) {
            return invalid_result(frame, ERROR_NOT_SUPPORTED, started);
        }
        PackedImage image = prepare_image(frame, dimension_limit, options.preferred_ocr_pixels);
        result.ocr_width = image.width;
        result.ocr_height = image.height;
        result.downscaled = image.width != frame.width || image.height != frame.height;

        if (image.pixels.size() > std::numeric_limits<std::uint32_t>::max()) {
            return invalid_result(frame, ERROR_FILE_TOO_LARGE, started);
        }
        Buffer buffer(static_cast<std::uint32_t>(image.pixels.size()));
        buffer.Length(static_cast<std::uint32_t>(image.pixels.size()));
        std::uint8_t* destination = nullptr;
        winrt::check_hresult(buffer.as<IBufferByteAccess>()->Buffer(&destination));
        std::memcpy(destination, image.pixels.data(), image.pixels.size());

        SoftwareBitmap bitmap(
            BitmapPixelFormat::Bgra8,
            static_cast<std::int32_t>(image.width),
            static_cast<std::int32_t>(image.height),
            BitmapAlphaMode::Ignore);
        bitmap.CopyFromBuffer(buffer);

        auto operation = engine.RecognizeAsync(bitmap);
        auto signal = std::make_shared<CompletionSignal>();
        if (signal->event == nullptr) {
            return invalid_result(frame, GetLastError(), started);
        }
        operation.Completed([signal](const auto&, const AsyncStatus) noexcept {
            SetEvent(signal->event);
        });

        const auto remaining = options.timeout
            - std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        const auto bounded_wait = std::min<std::int64_t>(
            std::max<std::int64_t>(remaining.count(), 0),
            std::numeric_limits<DWORD>::max() - 1ULL);
        const DWORD wait = bounded_wait == 0
            ? WAIT_TIMEOUT
            : WaitForSingleObject(signal->event, static_cast<DWORD>(bounded_wait));
        if (wait == WAIT_TIMEOUT) {
            operation.Cancel();
            result.timed_out = true;
            result.error = win32_error(ERROR_TIMEOUT);
        } else if (wait == WAIT_FAILED) {
            operation.Cancel();
            result.error = last_win32_error();
        } else if (operation.Status() == AsyncStatus::Completed) {
            auto recognized = operation.GetResults();
            std::vector<std::string> raw_lines;
            raw_lines.reserve(recognized.Lines().Size());
            for (const auto& line : recognized.Lines()) {
                raw_lines.push_back(winrt::to_string(line.Text()));
            }
            result.lines = normalize_ocr_lines(raw_lines, options.text_limits);
        } else if (operation.Status() == AsyncStatus::Error) {
            result.error = hresult_error(operation.ErrorCode());
        } else {
            result.error = win32_error(ERROR_CANCELLED);
        }
    } catch (const winrt::hresult_error& error) {
        result.error = hresult_error(error.code());
    } catch (const std::bad_alloc&) {
        result.error = win32_error(ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        result.error = win32_error(ERROR_UNHANDLED_EXCEPTION);
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

std::future<OcrRecognitionResult> ready_result(OcrRecognitionResult result) {
    std::promise<OcrRecognitionResult> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(result));
    return future;
}

}  // namespace

struct WindowsMediaOcr::SharedState {
    explicit SharedState(const std::size_t maximum) : maximum(maximum) {}
    std::atomic<std::size_t> active{0};
    std::size_t maximum{1};
};

WindowsMediaOcr::WindowsMediaOcr(const std::size_t max_concurrent_requests)
    : state_(std::make_shared<SharedState>(std::max<std::size_t>(1, max_concurrent_requests))) {}

std::future<OcrRecognitionResult> WindowsMediaOcr::recognize_async(
    ScreenFrame frame, OcrOptions options) const {
    std::size_t active = state_->active.load();
    do {
        if (active >= state_->maximum) {
            OcrRecognitionResult busy;
            busy.input_width = frame.width;
            busy.input_height = frame.height;
            busy.error = win32_error(ERROR_BUSY);
            return ready_result(std::move(busy));
        }
    } while (!state_->active.compare_exchange_weak(active, active + 1));

    auto promise = std::make_shared<std::promise<OcrRecognitionResult>>();
    auto future = promise->get_future();
    const auto state = state_;
    try {
        std::thread([state,
                     frame = std::move(frame),
                     options = std::move(options),
                     promise]() mutable {
            auto result = recognize_frame(std::move(frame), options);
            state->active.fetch_sub(1);
            promise->set_value(std::move(result));
        }).detach();
    } catch (const std::system_error& error) {
        state_->active.fetch_sub(1);
        OcrRecognitionResult failed;
        failed.error = error.code();
        promise->set_value(std::move(failed));
    }
    return future;
}

}  // namespace localflow::windows

#endif
