#include "localflow/core/text_chunker.hpp"

#include "utf8.hpp"

#include <stdexcept>

namespace localflow::core {
namespace {

bool is_hangul_l(char32_t value) noexcept {
    return (value >= 0x1100U && value <= 0x115FU)
        || (value >= 0xA960U && value <= 0xA97CU);
}

bool is_hangul_v(char32_t value) noexcept {
    return (value >= 0x1160U && value <= 0x11A7U)
        || (value >= 0xD7B0U && value <= 0xD7C6U);
}

bool is_hangul_t(char32_t value) noexcept {
    return (value >= 0x11A8U && value <= 0x11FFU)
        || (value >= 0xD7CBU && value <= 0xD7FBU);
}

bool is_hangul_lv(char32_t value) noexcept {
    return value >= 0xAC00U && value <= 0xD7A3U
        && (value - 0xAC00U) % 28U == 0U;
}

bool is_hangul_lvt(char32_t value) noexcept {
    return value >= 0xAC00U && value <= 0xD7A3U && !is_hangul_lv(value);
}

bool should_break(
    const std::vector<detail::CodePointSpan>& codepoints,
    std::size_t current) noexcept {
    const char32_t previous = codepoints[current - 1].value;
    const char32_t value = codepoints[current].value;

    if (previous == U'\r' && value == U'\n') {
        return false;
    }
    if (detail::is_grapheme_control(previous) || detail::is_grapheme_control(value)) {
        return true;
    }
    if (is_hangul_l(previous)
        && (is_hangul_l(value) || is_hangul_v(value)
            || is_hangul_lv(value) || is_hangul_lvt(value))) {
        return false;
    }
    if ((is_hangul_lv(previous) || is_hangul_v(previous))
        && (is_hangul_v(value) || is_hangul_t(value))) {
        return false;
    }
    if ((is_hangul_lvt(previous) || is_hangul_t(previous)) && is_hangul_t(value)) {
        return false;
    }
    if (detail::is_combining_mark(value)
        || detail::is_variation_selector(value)
        || detail::is_emoji_modifier(value)
        || value == 0x200DU) {  // Zero-width joiner.
        return false;
    }
    if (previous == 0x200DU && detail::is_extended_pictographic(value)) {
        return false;
    }
    if (detail::is_regional_indicator(previous)
        && detail::is_regional_indicator(value)) {
        std::size_t preceding_regional_indicators = 0;
        std::size_t cursor = current;
        while (cursor > 0 && detail::is_regional_indicator(codepoints[cursor - 1].value)) {
            ++preceding_regional_indicators;
            --cursor;
        }
        // Regional indicators form pairs: RI RI / RI RI.
        return preceding_regional_indicators % 2U == 0U;
    }
    return true;
}

struct Grapheme {
    std::size_t start{0};
    std::size_t end{0};
    std::size_t utf16_units{0};
};

std::vector<Grapheme> graphemes(const std::string& text) {
    const auto codepoints = detail::decode_utf8(text);
    std::vector<Grapheme> result;
    if (codepoints.empty()) {
        return result;
    }

    std::size_t cluster_start = 0;
    std::size_t units = detail::utf16_units(codepoints.front().value);
    for (std::size_t index = 1; index < codepoints.size(); ++index) {
        if (should_break(codepoints, index)) {
            result.push_back({
                codepoints[cluster_start].start,
                codepoints[index - 1].end,
                units,
            });
            cluster_start = index;
            units = 0;
        }
        units += detail::utf16_units(codepoints[index].value);
    }
    result.push_back({
        codepoints[cluster_start].start,
        codepoints.back().end,
        units,
    });
    return result;
}

}  // namespace

std::size_t utf16_length(const std::string& utf8_text) noexcept {
    std::size_t result = 0;
    for (const auto& span : detail::decode_utf8(utf8_text)) {
        result += detail::utf16_units(span.value);
    }
    return result;
}

std::vector<std::string> chunk_text_utf16(
    const std::string& text,
    std::size_t max_utf16_units_per_chunk) {
    if (max_utf16_units_per_chunk == 0) {
        throw std::invalid_argument("chunk size must be positive");
    }
    if (text.empty()) {
        return {};
    }

    std::vector<std::string> result;
    std::size_t current_start = 0;
    std::size_t current_end = 0;
    std::size_t current_units = 0;
    bool has_current = false;

    for (const auto& grapheme : graphemes(text)) {
        if (has_current
            && current_units + grapheme.utf16_units > max_utf16_units_per_chunk) {
            result.push_back(text.substr(current_start, current_end - current_start));
            has_current = false;
            current_units = 0;
        }
        if (!has_current) {
            current_start = grapheme.start;
            has_current = true;
        }
        current_end = grapheme.end;
        current_units += grapheme.utf16_units;
    }

    if (has_current) {
        result.push_back(text.substr(current_start, current_end - current_start));
    }
    return result;
}

}  // namespace localflow::core

