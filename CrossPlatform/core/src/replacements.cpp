#include "localflow/core/replacements.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace localflow::core {
namespace {

struct MatchSpan {
    std::size_t start_index{0};
    std::size_t end_index{0};  // exclusive code-point index
    std::size_t start_byte{0};
    std::size_t end_byte{0};
};

std::optional<MatchSpan> match_phrase_at(
    const std::vector<detail::CodePointSpan>& text,
    std::size_t start,
    const std::vector<detail::CodePointSpan>& pattern) {
    if (start >= text.size() || pattern.empty()) {
        return std::nullopt;
    }

    const bool first_is_word = detail::is_word_character(pattern.front().value);
    const bool previous_is_word = start > 0
        && detail::is_word_character(text[start - 1].value);
    if (first_is_word == previous_is_word) {
        return std::nullopt;
    }

    std::size_t text_index = start;
    std::size_t pattern_index = 0;
    while (pattern_index < pattern.size()) {
        if (detail::is_whitespace(pattern[pattern_index].value)) {
            while (pattern_index < pattern.size()
                   && detail::is_whitespace(pattern[pattern_index].value)) {
                ++pattern_index;
            }
            const std::size_t whitespace_start = text_index;
            while (text_index < text.size()
                   && detail::is_whitespace(text[text_index].value)) {
                ++text_index;
            }
            if (text_index == whitespace_start) {
                return std::nullopt;
            }
            continue;
        }
        if (text_index >= text.size()
            || detail::simple_lower(text[text_index].value)
                != detail::simple_lower(pattern[pattern_index].value)) {
            return std::nullopt;
        }
        ++text_index;
        ++pattern_index;
    }

    if (text_index == start) {
        return std::nullopt;
    }
    const bool last_is_word = detail::is_word_character(text[text_index - 1].value);
    const bool next_is_word = text_index < text.size()
        && detail::is_word_character(text[text_index].value);
    if (last_is_word == next_is_word) {
        return std::nullopt;
    }

    return MatchSpan{
        start,
        text_index,
        text[start].start,
        text[text_index - 1].end,
    };
}

char32_t simple_upper(char32_t value) noexcept {
    if (value >= U'a' && value <= U'z') {
        return value - 0x20U;
    }
    if ((value >= 0x00E0U && value <= 0x00F6U)
        || (value >= 0x00F8U && value <= 0x00FEU)) {
        return value - 0x20U;
    }
    if (value >= 0x0101U && value <= 0x017FU && value % 2U == 1U) {
        return value - 1U;
    }
    if ((value >= 0x03B1U && value <= 0x03C1U)
        || (value >= 0x03C3U && value <= 0x03C9U)
        || (value >= 0x0430U && value <= 0x044FU)) {
        return value - 0x20U;
    }
    return value;
}

std::string uppercase(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto& point : detail::decode_utf8(value)) {
        result += point.valid
            ? detail::encode_utf8(simple_upper(point.value))
            : value.substr(point.start, point.end - point.start);
    }
    return result;
}

std::string capitalize_first(const std::string& value) {
    const auto points = detail::decode_utf8(value);
    if (points.empty() || !points.front().valid) {
        return value;
    }
    return detail::encode_utf8(simple_upper(points.front().value))
        + value.substr(points.front().end);
}

std::string adapt_case(const std::string& written, const std::string& matched) {
    const auto written_points = detail::decode_utf8(written);
    if (std::any_of(written_points.begin(), written_points.end(), [](const auto& point) {
            return detail::is_upper(point.value);
        })) {
        return written;
    }

    std::vector<char32_t> letters;
    for (const auto& point : detail::decode_utf8(matched)) {
        if (detail::is_letter(point.value)) {
            letters.push_back(point.value);
        }
    }
    if (letters.empty()) {
        return written;
    }
    if (letters.size() > 1
        && std::all_of(letters.begin(), letters.end(), detail::is_upper)) {
        return uppercase(written);
    }
    if (detail::is_upper(letters.front())) {
        return capitalize_first(written);
    }
    return written;
}

struct TextReplacement {
    std::size_t start{0};
    std::size_t end{0};
    std::string replacement;
    std::size_t priority{0};
};

std::string apply_spoken_punctuation(const std::string& input) {
    struct Command {
        std::string_view phrase;
        std::string_view replacement;
        bool consumes_trailing_space;
    };
    static constexpr Command commands[] = {
        {"new paragraph", "\n\n", true},
        {"question mark", "?", false},
        {"exclamation mark", "!", false},
        {"exclamation point", "!", false},
        {"new line", "\n", true},
        {"semicolon", ";", false},
        {"period", ".", false},
        {"comma", ",", false},
        {"colon", ":", false},
    };

    std::string result = input;
    for (const auto& command : commands) {
        const auto points = detail::decode_utf8(result);
        const auto pattern = detail::decode_utf8(command.phrase);
        std::vector<TextReplacement> replacements;
        std::size_t cursor = 0;
        while (cursor < points.size()) {
            const auto match = match_phrase_at(points, cursor, pattern);
            if (!match) {
                ++cursor;
                continue;
            }
            std::size_t first = match->start_index;
            while (first > 0 && detail::is_whitespace(points[first - 1].value)) {
                --first;
            }
            std::size_t last = match->end_index;
            if (command.consumes_trailing_space) {
                while (last < points.size()
                       && (points[last].value == U' ' || points[last].value == U'\t')) {
                    ++last;
                }
            }
            replacements.push_back({
                points[first].start,
                last == points.size() ? result.size() : points[last].start,
                std::string(command.replacement),
                0,
            });
            cursor = last;
        }
        if (replacements.empty()) {
            continue;
        }
        std::string output;
        std::size_t byte_cursor = 0;
        for (const auto& replacement : replacements) {
            output.append(result, byte_cursor, replacement.start - byte_cursor);
            output += replacement.replacement;
            byte_cursor = replacement.end;
        }
        output.append(result, byte_cursor, result.size() - byte_cursor);
        result = std::move(output);
    }
    return result;
}

}  // namespace

ReplacementEngine::ReplacementEngine(PersonalDictionary dictionary)
    : dictionary_(std::move(dictionary)) {
    struct Pending {
        ReplacementRule rule;
        std::size_t original_index{0};
    };
    std::vector<Pending> pending;
    pending.reserve(dictionary_.rules.size());
    for (std::size_t index = 0; index < dictionary_.rules.size(); ++index) {
        const auto spoken = detail::trim_unicode_whitespace(dictionary_.rules[index].spoken);
        if (!spoken.empty()) {
            pending.push_back({{spoken, dictionary_.rules[index].written}, index});
        }
    }
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& lhs, const Pending& rhs) {
        return detail::codepoint_count(lhs.rule.spoken)
            > detail::codepoint_count(rhs.rule.spoken);
    });
    compiled_.reserve(pending.size());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        compiled_.push_back({
            pending[index].rule.spoken,
            pending[index].rule.written,
            detail::codepoint_count(pending[index].rule.spoken),
            index,
        });
    }
}

const PersonalDictionary& ReplacementEngine::dictionary() const noexcept {
    return dictionary_;
}

std::string ReplacementEngine::apply(const std::string& text) const {
    std::string result = text;
    if (!compiled_.empty() && !text.empty()) {
        const auto points = detail::decode_utf8(text);
        std::vector<TextReplacement> candidates;
        for (const auto& rule : compiled_) {
            const auto pattern = detail::decode_utf8(rule.spoken);
            for (std::size_t index = 0; index < points.size(); ++index) {
                if (const auto match = match_phrase_at(points, index, pattern)) {
                    const auto matched = text.substr(
                        match->start_byte,
                        match->end_byte - match->start_byte);
                    candidates.push_back({
                        match->start_byte,
                        match->end_byte,
                        adapt_case(rule.written, matched),
                        rule.priority,
                    });
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.start != rhs.start) {
                return lhs.start < rhs.start;
            }
            const auto lhs_length = lhs.end - lhs.start;
            const auto rhs_length = rhs.end - rhs.start;
            if (lhs_length != rhs_length) {
                return lhs_length > rhs_length;
            }
            return lhs.priority < rhs.priority;
        });

        std::vector<TextReplacement> accepted;
        std::size_t cursor = 0;
        for (const auto& candidate : candidates) {
            if (candidate.start >= cursor) {
                accepted.push_back(candidate);
                cursor = candidate.end;
            }
        }
        if (!accepted.empty()) {
            std::string output;
            cursor = 0;
            for (const auto& replacement : accepted) {
                output.append(text, cursor, replacement.start - cursor);
                output += replacement.replacement;
                cursor = replacement.end;
            }
            output.append(text, cursor, text.size() - cursor);
            result = std::move(output);
        }
    }

    if (dictionary_.spoken_punctuation_enabled) {
        result = apply_spoken_punctuation(result);
    }
    return result;
}

}  // namespace localflow::core

