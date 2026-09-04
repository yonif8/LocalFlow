#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace localflow::core::detail {
namespace {

constexpr bool in(char32_t value, char32_t first, char32_t last) noexcept {
    return value >= first && value <= last;
}

bool is_continuation(unsigned char byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

}  // namespace

std::vector<CodePointSpan> decode_utf8(std::string_view text) {
    std::vector<CodePointSpan> result;
    result.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t width = 1;
        char32_t value = first;
        bool valid = true;
        if (first < 0x80U) {
            width = 1;
        } else if ((first & 0xE0U) == 0xC0U) {
            width = 2;
            value = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            width = 3;
            value = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            width = 4;
            value = first & 0x07U;
        } else {
            valid = false;
        }

        if (valid && index + width <= text.size()) {
            for (std::size_t offset = 1; offset < width; ++offset) {
                const auto byte = static_cast<unsigned char>(text[index + offset]);
                if (!is_continuation(byte)) {
                    valid = false;
                    break;
                }
                value = (value << 6U) | (byte & 0x3FU);
            }
            const bool overlong = (width == 2 && value < 0x80U)
                || (width == 3 && value < 0x800U)
                || (width == 4 && value < 0x10000U);
            if (overlong || in(value, 0xD800U, 0xDFFFU) || value > 0x10FFFFU) {
                valid = false;
            }
        } else if (valid) {
            valid = false;
        }

        if (!valid) {
            // Preserve malformed input byte-for-byte. Values above Unicode's
            // range are internal sentinels and never get encoded.
            result.push_back({static_cast<char32_t>(0x110000U + first), index, index + 1, false});
            ++index;
        } else {
            result.push_back({value, index, index + width, true});
            index += width;
        }
    }
    return result;
}

std::string encode_utf8(char32_t value) {
    std::string output;
    if (value <= 0x7FU) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0xFFFFU && !in(value, 0xD800U, 0xDFFFU)) {
        output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
    return output;
}

bool is_number(char32_t value) noexcept {
    return in(value, U'0', U'9')
        || in(value, 0x0660U, 0x0669U)
        || in(value, 0x06F0U, 0x06F9U)
        || in(value, 0x0966U, 0x096FU)
        || in(value, 0x09E6U, 0x09EFU)
        || in(value, 0x0A66U, 0x0A6FU)
        || in(value, 0x0AE6U, 0x0AEFU)
        || in(value, 0x0B66U, 0x0B6FU)
        || in(value, 0x0BE6U, 0x0BEFU)
        || in(value, 0x0C66U, 0x0C6FU)
        || in(value, 0x0CE6U, 0x0CEFU)
        || in(value, 0x0D66U, 0x0D6FU)
        || in(value, 0x0E50U, 0x0E59U)
        || in(value, 0x0ED0U, 0x0ED9U)
        || in(value, 0x1040U, 0x1049U)
        || in(value, 0xFF10U, 0xFF19U);
}

bool is_upper(char32_t value) noexcept {
    return in(value, U'A', U'Z')
        || in(value, 0x00C0U, 0x00D6U)
        || in(value, 0x00D8U, 0x00DEU)
        || (in(value, 0x0100U, 0x017EU) && value % 2U == 0U)
        || in(value, 0x0391U, 0x03A9U)
        || in(value, 0x0410U, 0x042FU)
        || in(value, 0x0400U, 0x040FU);
}

bool is_lower(char32_t value) noexcept {
    return in(value, U'a', U'z')
        || in(value, 0x00DFU, 0x00F6U)
        || in(value, 0x00F8U, 0x00FFU)
        || (in(value, 0x0101U, 0x017FU) && value % 2U == 1U)
        || in(value, 0x03B1U, 0x03C9U)
        || in(value, 0x0430U, 0x044FU)
        || in(value, 0x0450U, 0x045FU);
}

bool is_letter(char32_t value) noexcept {
    if (is_upper(value) || is_lower(value)) {
        return true;
    }
    return in(value, 0x0180U, 0x02AFU)       // Latin extensions and IPA
        || in(value, 0x0370U, 0x052FU)       // Greek and Cyrillic
        || in(value, 0x0531U, 0x0588U)       // Armenian
        || in(value, 0x05D0U, 0x05EAU)       // Hebrew
        || in(value, 0x0620U, 0x06FFU)       // Arabic
        || in(value, 0x0710U, 0x074FU)       // Syriac
        || in(value, 0x0780U, 0x07BFU)
        || in(value, 0x0904U, 0x0D7FU)       // Indic scripts
        || in(value, 0x0E01U, 0x0FFFU)       // Thai through Tibetan
        || in(value, 0x1000U, 0x109FU)       // Myanmar
        || in(value, 0x10A0U, 0x10FFU)       // Georgian
        || in(value, 0x1100U, 0x11FFU)       // Hangul Jamo
        || in(value, 0x1200U, 0x18AFU)
        || in(value, 0x3041U, 0x30FFU)       // Hiragana and Katakana
        || in(value, 0x3105U, 0x312FU)       // Bopomofo
        || in(value, 0x3400U, 0x4DBFU)
        || in(value, 0x4E00U, 0x9FFFU)       // CJK
        || in(value, 0xA000U, 0xA4CFU)
        || in(value, 0xAC00U, 0xD7A3U)       // Hangul syllables
        || in(value, 0xF900U, 0xFAFFU)
        || in(value, 0xFF21U, 0xFF3AU)
        || in(value, 0xFF41U, 0xFF5AU)
        || in(value, 0x10000U, 0x1EFFFU);    // historic/supplementary scripts
}

bool is_whitespace(char32_t value) noexcept {
    return value == U' ' || in(value, U'\t', U'\r')
        || value == 0x0085U || value == 0x00A0U || value == 0x1680U
        || in(value, 0x2000U, 0x200AU)
        || value == 0x2028U || value == 0x2029U || value == 0x202FU
        || value == 0x205FU || value == 0x3000U;
}

bool is_word_character(char32_t value) noexcept {
    return is_letter(value) || is_number(value) || value == U'_';
}

bool is_combining_mark(char32_t value) noexcept {
    return in(value, 0x0300U, 0x036FU)
        || in(value, 0x0483U, 0x0489U)
        || in(value, 0x0591U, 0x05BDU)
        || value == 0x05BFU || in(value, 0x05C1U, 0x05C2U)
        || in(value, 0x0610U, 0x061AU)
        || in(value, 0x064BU, 0x065FU)
        || in(value, 0x0900U, 0x0903U)
        || in(value, 0x093AU, 0x094FU)
        || in(value, 0x1AB0U, 0x1AFFU)
        || in(value, 0x1DC0U, 0x1DFFU)
        || in(value, 0x20D0U, 0x20FFU)
        || in(value, 0xFE20U, 0xFE2FU);
}

bool is_variation_selector(char32_t value) noexcept {
    return in(value, 0xFE00U, 0xFE0FU) || in(value, 0xE0100U, 0xE01EFU);
}

bool is_emoji_modifier(char32_t value) noexcept {
    return in(value, 0x1F3FBU, 0x1F3FFU);
}

bool is_regional_indicator(char32_t value) noexcept {
    return in(value, 0x1F1E6U, 0x1F1FFU);
}

bool is_extended_pictographic(char32_t value) noexcept {
    return in(value, 0x1F000U, 0x1FAFFU)
        || in(value, 0x2600U, 0x27BFU)
        || in(value, 0x2300U, 0x23FFU);
}

bool is_grapheme_control(char32_t value) noexcept {
    return value == U'\r' || value == U'\n'
        || value < 0x20U || in(value, 0x007FU, 0x009FU)
        || value == 0x2028U || value == 0x2029U;
}

char32_t simple_lower(char32_t value) noexcept {
    if (in(value, U'A', U'Z')) {
        return value + 0x20U;
    }
    if (in(value, 0x00C0U, 0x00D6U) || in(value, 0x00D8U, 0x00DEU)) {
        return value + 0x20U;
    }
    if (in(value, 0x0100U, 0x017EU) && value % 2U == 0U) {
        return value + 1U;
    }
    if (in(value, 0x0391U, 0x03A1U) || in(value, 0x03A3U, 0x03A9U)) {
        return value + 0x20U;
    }
    if (in(value, 0x0410U, 0x042FU)) {
        return value + 0x20U;
    }
    if (in(value, 0x0400U, 0x040FU)) {
        return value + 0x50U;
    }
    if (in(value, 0xFF21U, 0xFF3AU)) {
        return value + 0x20U;
    }
    return value;
}

char32_t latin_base_letter(char32_t value) noexcept {
    value = simple_lower(value);
    switch (value) {
    case U'\u00e0': case U'\u00e1': case U'\u00e2': case U'\u00e3':
    case U'\u00e4': case U'\u00e5': case U'\u0101': case U'\u0103':
    case U'\u0105': return U'a';
    case U'\u00e7': case U'\u0107': case U'\u0109': case U'\u010b':
    case U'\u010d': return U'c';
    case U'\u010f': case U'\u0111': return U'd';
    case U'\u00e8': case U'\u00e9': case U'\u00ea': case U'\u00eb':
    case U'\u0113': case U'\u0115': case U'\u0117': case U'\u0119':
    case U'\u011b': return U'e';
    case U'\u011d': case U'\u011f': case U'\u0121': case U'\u0123': return U'g';
    case U'\u0125': case U'\u0127': return U'h';
    case U'\u00ec': case U'\u00ed': case U'\u00ee': case U'\u00ef':
    case U'\u0129': case U'\u012b': case U'\u012d': case U'\u012f':
    case U'\u0131': return U'i';
    case U'\u0135': return U'j';
    case U'\u0137': return U'k';
    case U'\u013a': case U'\u013c': case U'\u013e': case U'\u0140':
    case U'\u0142': return U'l';
    case U'\u00f1': case U'\u0144': case U'\u0146': case U'\u0148': return U'n';
    case U'\u00f2': case U'\u00f3': case U'\u00f4': case U'\u00f5':
    case U'\u00f6': case U'\u00f8': case U'\u014d': case U'\u014f':
    case U'\u0151': return U'o';
    case U'\u0155': case U'\u0157': case U'\u0159': return U'r';
    case U'\u015b': case U'\u015d': case U'\u015f': case U'\u0161': return U's';
    case U'\u0163': case U'\u0165': case U'\u0167': return U't';
    case U'\u00f9': case U'\u00fa': case U'\u00fb': case U'\u00fc':
    case U'\u0169': case U'\u016b': case U'\u016d': case U'\u016f':
    case U'\u0171': case U'\u0173': return U'u';
    case U'\u0175': return U'w';
    case U'\u00fd': case U'\u00ff': case U'\u0177': return U'y';
    case U'\u017a': case U'\u017c': case U'\u017e': return U'z';
    default: return value;
    }
}

std::size_t codepoint_count(std::string_view text) noexcept {
    return decode_utf8(text).size();
}

std::size_t utf16_units(char32_t value) noexcept {
    return value <= 0xFFFFU ? 1U : (value <= 0x10FFFFU ? 2U : 1U);
}

std::string trim_unicode_whitespace(std::string_view text) {
    const auto codepoints = decode_utf8(text);
    if (codepoints.empty()) {
        return {};
    }
    std::size_t first = 0;
    while (first < codepoints.size() && is_whitespace(codepoints[first].value)) {
        ++first;
    }
    if (first == codepoints.size()) {
        return {};
    }
    std::size_t last = codepoints.size();
    while (last > first && is_whitespace(codepoints[last - 1].value)) {
        --last;
    }
    return std::string(text.substr(
        codepoints[first].start,
        codepoints[last - 1].end - codepoints[first].start));
}

std::vector<std::string> split_whitespace(std::string_view text) {
    const auto codepoints = decode_utf8(text);
    std::vector<std::string> parts;
    std::size_t index = 0;
    while (index < codepoints.size()) {
        while (index < codepoints.size() && is_whitespace(codepoints[index].value)) {
            ++index;
        }
        if (index == codepoints.size()) {
            break;
        }
        const std::size_t start = codepoints[index].start;
        while (index < codepoints.size() && !is_whitespace(codepoints[index].value)) {
            ++index;
        }
        const std::size_t end = codepoints[index - 1].end;
        parts.emplace_back(text.substr(start, end - start));
    }
    return parts;
}

std::string simple_case_fold(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto& span : decode_utf8(text)) {
        if (!span.valid) {
            result.append(text.substr(span.start, span.end - span.start));
        } else {
            result += encode_utf8(simple_lower(span.value));
        }
    }
    return result;
}

bool case_insensitive_equal(std::string_view lhs, std::string_view rhs) {
    return simple_case_fold(lhs) == simple_case_fold(rhs);
}

}  // namespace localflow::core::detail

