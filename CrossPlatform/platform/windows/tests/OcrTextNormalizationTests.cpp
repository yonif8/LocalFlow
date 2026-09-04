#include "localflow/windows/OcrTextNormalization.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void preserves_terminology_exactly() {
    const auto lines = localflow::windows::normalize_ocr_lines(
        {"  PostgreSQL\tS1-mini  ", "LFPolish / Terminology.swift", "ALL_CAPS"});
    expect(lines.size() == 3, "all meaningful lines remain");
    expect(lines[0] == "PostgreSQL S1-mini", "only whitespace is normalized");
    expect(lines[1] == "LFPolish / Terminology.swift", "path and punctuation remain exact");
    expect(lines[2] == "ALL_CAPS", "capitalization remains exact");
}

void removes_empty_and_exact_duplicate_lines() {
    const auto lines = localflow::windows::normalize_ocr_lines(
        {"", " \r\n\t", "Telegram", "Telegram", "telegram"});
    expect(lines.size() == 2, "empty and exact duplicate lines are removed");
    expect(lines[0] == "Telegram" && lines[1] == "telegram",
           "deduplication remains case-sensitive");
}

void enforces_all_limits() {
    localflow::windows::OcrTextNormalizationOptions options;
    options.max_lines = 2;
    options.max_bytes_per_line = 4;
    options.max_total_bytes = 7;
    const auto lines = localflow::windows::normalize_ocr_lines(
        {"abcdef", "xyz", "ignored"}, options);
    expect(lines.size() == 2, "line-count limit is enforced");
    expect(lines[0] == "abcd" && lines[1] == "xyz", "line and total byte limits are enforced");
}

void never_splits_utf8_code_points() {
    localflow::windows::OcrTextNormalizationOptions options;
    options.max_bytes_per_line = 4;
    const auto lines = localflow::windows::normalize_ocr_lines({"abc\xC3\xA9-more"}, options);
    expect(lines.size() == 1, "bounded Unicode line remains present");
    expect(lines[0] == "abc", "truncation excludes an incomplete UTF-8 code point");
}

}  // namespace

int main() {
    preserves_terminology_exactly();
    removes_empty_and_exact_duplicate_lines();
    enforces_all_limits();
    never_splits_utf8_code_points();
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "OcrTextNormalizationTests: all assertions passed\n";
    return EXIT_SUCCESS;
}
