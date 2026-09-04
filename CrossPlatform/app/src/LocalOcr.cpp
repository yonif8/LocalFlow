#include "LocalOcr.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

#if LOCALFLOW_HAVE_TESSERACT
#include <tesseract/baseapi.h>
#endif

namespace {
constexpr int kMaxSourceDimension = 16384;
constexpr std::uint64_t kMaxSourcePixels = 64ULL * 1024ULL * 1024ULL;
constexpr int kPreferredDimension = 2400;
constexpr std::uint64_t kPreferredPixels = 6ULL * 1024ULL * 1024ULL;

std::string clean_line(std::string value) {
    std::string output;
    output.reserve(std::min<std::size_t>(value.size(), 256));
    bool pendingSpace = false;
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7fU) {
            pendingSpace = !output.empty();
            continue;
        }
        if (byte == ' ' || byte == '\t') {
            pendingSpace = !output.empty();
            continue;
        }
        if (pendingSpace && output.size() < 256) output.push_back(' ');
        pendingSpace = false;
        if (output.size() < 256) output.push_back(char(byte));
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}
}

bool LocalOcr::available() noexcept {
#if LOCALFLOW_HAVE_TESSERACT
    return true;
#else
    return false;
#endif
}

LocalOcrResult LocalOcr::recognize(const LocalOcrFrame& frame) {
    const auto started = std::chrono::steady_clock::now();
    LocalOcrResult result;
    const auto finish = [&] {
        result.elapsedMs = int(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        return result;
    };
#if !LOCALFLOW_HAVE_TESSERACT
    result.error = "This build does not include the local OCR runtime";
    return finish();
#else
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < frame.width * 4
        || frame.width > kMaxSourceDimension || frame.height > kMaxSourceDimension
        || std::uint64_t(frame.width) * std::uint64_t(frame.height) > kMaxSourcePixels
        || std::uint64_t(frame.stride) * std::uint64_t(frame.height) > frame.pixels.size()) {
        result.error = "Invalid or oversized screen frame";
        return finish();
    }

    double scale = 1.0;
    const int longest = std::max(frame.width, frame.height);
    if (longest > kPreferredDimension) scale = double(kPreferredDimension) / double(longest);
    const auto sourcePixels = std::uint64_t(frame.width) * std::uint64_t(frame.height);
    if (sourcePixels > kPreferredPixels) {
        scale = std::min(scale, std::sqrt(double(kPreferredPixels) / double(sourcePixels)));
    }
    const int width = std::max(1, int(std::floor(frame.width * scale)));
    const int height = std::max(1, int(std::floor(frame.height * scale)));
    result.downscaled = width != frame.width || height != frame.height;
    std::vector<std::uint8_t> grayscale(std::size_t(width) * std::size_t(height));
    const double xRatio = double(frame.width) / width;
    const double yRatio = double(frame.height) / height;
    for (int y = 0; y < height; ++y) {
        const int sourceY = std::min(frame.height - 1, int((y + 0.5) * yRatio));
        for (int x = 0; x < width; ++x) {
            const int sourceX = std::min(frame.width - 1, int((x + 0.5) * xRatio));
            const auto* pixel = frame.pixels.data() + std::size_t(sourceY) * frame.stride
                + std::size_t(sourceX) * 4;
            const int red = frame.bgra ? pixel[2] : pixel[0];
            const int green = pixel[1];
            const int blue = frame.bgra ? pixel[0] : pixel[2];
            grayscale[std::size_t(y) * width + x] = std::uint8_t(
                (red * 77 + green * 150 + blue * 29) >> 8);
        }
    }

    tesseract::TessBaseAPI engine;
    if (engine.Init(nullptr, "eng", tesseract::OEM_LSTM_ONLY) != 0) {
        result.error = "Could not initialize the bundled English OCR data";
        return finish();
    }
    engine.SetPageSegMode(tesseract::PSM_AUTO);
    engine.SetVariable("preserve_interword_spaces", "1");
    engine.SetImage(grayscale.data(), width, height, 1, width);
    if (engine.Recognize(nullptr) != 0) {
        result.error = "Local OCR could not recognize this screen";
        return finish();
    }
    char* raw = engine.GetUTF8Text();
    if (raw == nullptr) {
        result.error = "Local OCR returned no text";
        return finish();
    }
    const std::string text(raw);
    delete[] raw;
    std::istringstream stream(text);
    std::string line;
    std::size_t totalBytes = 0;
    while (result.lines.size() < 400 && std::getline(stream, line)) {
        line = clean_line(std::move(line));
        if (line.empty()) continue;
        totalBytes += line.size();
        if (totalBytes > 32768) break;
        result.lines.push_back(std::move(line));
    }
    return finish();
#endif
}
