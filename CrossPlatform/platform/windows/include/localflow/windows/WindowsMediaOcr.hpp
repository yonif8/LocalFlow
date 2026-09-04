#pragma once

#ifdef _WIN32

#include "localflow/windows/OcrTextNormalization.hpp"
#include "localflow/windows/ScreenCapture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace localflow::windows {

struct OcrOptions {
    /// Null uses the user's installed recognition languages. A BCP-47 tag such
    /// as "en-US" requests one installed Windows OCR language explicitly.
    std::optional<std::string> language_tag;
    std::chrono::milliseconds timeout{1500};

    // Source limits reject corrupt/untrusted frame metadata before allocation.
    std::uint32_t max_source_dimension{16'384};
    std::uint64_t max_source_pixels{64ULL * 1024ULL * 1024ULL};
    std::size_t max_source_bytes{256ULL * 1024ULL * 1024ULL};

    // Larger frames are locally downscaled before recognition. The Windows OCR
    // runtime's own MaxImageDimension is applied as an additional upper bound.
    std::uint32_t preferred_ocr_dimension{2400};
    std::uint64_t preferred_ocr_pixels{6ULL * 1024ULL * 1024ULL};
    OcrTextNormalizationOptions text_limits{};
};

struct OcrRecognitionResult {
    std::vector<std::string> lines;
    std::string recognition_language;
    std::chrono::milliseconds elapsed{0};
    std::uint32_t input_width{0};
    std::uint32_t input_height{0};
    std::uint32_t ocr_width{0};
    std::uint32_t ocr_height{0};
    bool downscaled{false};
    bool timed_out{false};
    std::error_code error;

    [[nodiscard]] bool succeeded() const noexcept { return !error; }
};

/// Asynchronous, entirely on-device Windows.Media.Ocr recognizer.
///
/// Each request owns its frame and WinRT apartment. Destroying a returned
/// future never waits for OCR. Requests are bounded and the underlying WinRT
/// operation is cancelled when its deadline expires.
class WindowsMediaOcr final {
public:
    explicit WindowsMediaOcr(std::size_t max_concurrent_requests = 1);
    ~WindowsMediaOcr() = default;

    WindowsMediaOcr(const WindowsMediaOcr&) = delete;
    WindowsMediaOcr& operator=(const WindowsMediaOcr&) = delete;

    [[nodiscard]] std::future<OcrRecognitionResult> recognize_async(
        ScreenFrame frame, OcrOptions options = {}) const;

private:
    struct SharedState;
    std::shared_ptr<SharedState> state_;
};

}  // namespace localflow::windows

#endif
