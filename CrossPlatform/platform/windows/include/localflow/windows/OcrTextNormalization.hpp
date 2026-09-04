#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace localflow::windows {

struct OcrTextNormalizationOptions {
    std::size_t max_lines{512};
    std::size_t max_bytes_per_line{4 * 1024};
    std::size_t max_total_bytes{1024 * 1024};
    bool remove_exact_duplicates{true};
};

/// Bounds noisy OCR output while preserving Unicode, spelling, punctuation,
/// and case exactly. Only ASCII control/whitespace cleanup is performed; term
/// interpretation belongs to the shared conservative terminology extractor.
[[nodiscard]] std::vector<std::string> normalize_ocr_lines(
    const std::vector<std::string>& raw_lines,
    const OcrTextNormalizationOptions& options = {});

}  // namespace localflow::windows
