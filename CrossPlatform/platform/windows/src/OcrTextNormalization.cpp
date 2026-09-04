#include "localflow/windows/OcrTextNormalization.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>

namespace localflow::windows {
namespace {

std::string normalize_line(const std::string_view raw) {
    std::string result;
    result.reserve(raw.size());
    bool pending_space = false;

    for (const unsigned char byte : raw) {
        if (byte <= 0x20U) {
            pending_space = !result.empty();
            continue;
        }
        if (byte == 0x7FU) {
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

void truncate_at_utf8_boundary(std::string& value, const std::size_t limit) {
    if (value.size() <= limit) {
        return;
    }
    std::size_t end = limit;
    while (end > 0
           && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    value.resize(end);
}

}  // namespace

std::vector<std::string> normalize_ocr_lines(
    const std::vector<std::string>& raw_lines,
    const OcrTextNormalizationOptions& options) {
    std::vector<std::string> result;
    if (options.max_lines == 0 || options.max_bytes_per_line == 0
        || options.max_total_bytes == 0) {
        return result;
    }
    result.reserve(std::min(raw_lines.size(), options.max_lines));
    std::unordered_set<std::string> seen;
    std::size_t total_bytes = 0;

    for (const auto& raw : raw_lines) {
        if (result.size() >= options.max_lines || total_bytes >= options.max_total_bytes) {
            break;
        }
        std::string line = normalize_line(raw);
        const std::size_t remaining = options.max_total_bytes - total_bytes;
        truncate_at_utf8_boundary(line, std::min(options.max_bytes_per_line, remaining));
        if (line.empty()) {
            continue;
        }
        if (options.remove_exact_duplicates && !seen.insert(line).second) {
            continue;
        }
        total_bytes += line.size();
        result.push_back(std::move(line));
    }
    return result;
}

}  // namespace localflow::windows
