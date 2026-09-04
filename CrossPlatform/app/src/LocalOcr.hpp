#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct LocalOcrFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    bool bgra = true;
    std::vector<std::uint8_t> pixels;
};

struct LocalOcrResult {
    std::vector<std::string> lines;
    std::string error;
    int elapsedMs = 0;
    bool downscaled = false;
};

class LocalOcr final {
public:
    static bool available() noexcept;
    static LocalOcrResult recognize(const LocalOcrFrame& frame);
};
