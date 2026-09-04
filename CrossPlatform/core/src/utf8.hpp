#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace localflow::core::detail {

struct CodePointSpan {
    char32_t value{0};
    std::size_t start{0};
    std::size_t end{0};
    bool valid{true};
};

[[nodiscard]] std::vector<CodePointSpan> decode_utf8(std::string_view text);
[[nodiscard]] std::string encode_utf8(char32_t value);

[[nodiscard]] bool is_letter(char32_t value) noexcept;
[[nodiscard]] bool is_number(char32_t value) noexcept;
[[nodiscard]] bool is_upper(char32_t value) noexcept;
[[nodiscard]] bool is_lower(char32_t value) noexcept;
[[nodiscard]] bool is_whitespace(char32_t value) noexcept;
[[nodiscard]] bool is_word_character(char32_t value) noexcept;
[[nodiscard]] bool is_combining_mark(char32_t value) noexcept;
[[nodiscard]] bool is_variation_selector(char32_t value) noexcept;
[[nodiscard]] bool is_emoji_modifier(char32_t value) noexcept;
[[nodiscard]] bool is_regional_indicator(char32_t value) noexcept;
[[nodiscard]] bool is_extended_pictographic(char32_t value) noexcept;
[[nodiscard]] bool is_grapheme_control(char32_t value) noexcept;
[[nodiscard]] char32_t simple_lower(char32_t value) noexcept;
[[nodiscard]] char32_t latin_base_letter(char32_t value) noexcept;

[[nodiscard]] std::size_t codepoint_count(std::string_view text) noexcept;
[[nodiscard]] std::size_t utf16_units(char32_t value) noexcept;
[[nodiscard]] std::string trim_unicode_whitespace(std::string_view text);
[[nodiscard]] std::vector<std::string> split_whitespace(std::string_view text);
[[nodiscard]] std::string simple_case_fold(std::string_view text);
[[nodiscard]] bool case_insensitive_equal(std::string_view lhs, std::string_view rhs);

}  // namespace localflow::core::detail

