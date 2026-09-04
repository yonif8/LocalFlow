#include "localflow/core/terminology.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace localflow::core {
namespace {

const std::unordered_set<std::string>& common_capitalized() {
    static const std::unordered_set<std::string> words = {
        "a", "about", "account", "add", "also", "am", "an", "and", "are", "as", "at",
        "back", "be", "because", "but", "by", "can", "cancel", "chat", "check", "close",
        "configure", "connect", "continue", "copy", "could", "delete", "did", "do", "does",
        "done", "edit", "file", "find", "for", "from", "general", "go", "had", "has",
        "have", "he", "help", "her", "him", "his", "home", "how", "i", "if", "in",
        "is", "it", "its", "just", "learn", "may", "menu", "might", "my", "new", "next",
        "no", "not", "of", "on", "open", "or", "our", "preferences", "remove", "save",
        "search", "settings", "share", "she", "should", "so", "the", "their", "them", "then",
        "there", "these", "they", "this", "those", "to", "today", "tomorrow", "tools", "view",
        "was", "we", "were", "what", "when", "where", "which", "who", "why", "will", "window",
        "with", "would", "yes", "you", "your",
    };
    return words;
}

const std::unordered_set<std::string>& name_connectors() {
    static const std::unordered_set<std::string> words = {
        "al", "and", "da", "de", "del", "den", "der", "di", "el", "of", "the", "to", "van", "von",
    };
    return words;
}

bool contains_ascii(std::string_view value, std::string_view characters) {
    return std::any_of(value.begin(), value.end(), [characters](char character) {
        return static_cast<unsigned char>(character) < 0x80U
            && characters.find(character) != std::string_view::npos;
    });
}

std::string lowercase(std::string_view value) {
    return detail::simple_case_fold(value);
}

bool is_common(std::string_view value) {
    return common_capitalized().count(lowercase(value)) != 0;
}

bool title_cased(std::string_view token) {
    const auto points = detail::decode_utf8(token);
    if (points.size() < 3 || !detail::is_upper(points.front().value)) {
        return false;
    }
    return std::any_of(points.begin() + 1, points.end(), [](const auto& point) {
        return detail::is_lower(point.value);
    });
}

std::vector<std::string> split_on(std::string_view value, char separator) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(separator, start);
        const auto length = (end == std::string_view::npos ? value.size() : end) - start;
        if (length != 0) {
            result.emplace_back(value.substr(start, length));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

bool distinctive(std::string_view token) {
    const auto points = detail::decode_utf8(token);
    std::vector<char32_t> letters;
    bool has_number = false;
    for (const auto& point : points) {
        if (detail::is_letter(point.value)) {
            letters.push_back(point.value);
        }
        has_number = has_number || detail::is_number(point.value);
    }
    const bool has_upper = std::any_of(letters.begin(), letters.end(), detail::is_upper);
    const bool has_lower = std::any_of(letters.begin(), letters.end(), detail::is_lower);
    const bool inner_upper = has_lower && points.size() > 1
        && std::any_of(points.begin() + 1, points.end(), [](const auto& point) {
            return detail::is_upper(point.value);
        });
    const bool acronym = letters.size() >= 2 && letters.size() <= 6
        && has_upper && !has_lower && !is_common(token);
    const bool alphanumeric = has_number && !letters.empty();
    const bool technical_punctuation = contains_ascii(token, "._+#/\\");

    const auto hyphenated = split_on(token, '-');
    const bool connector_name = hyphenated.size() == 2
        && name_connectors().count(lowercase(hyphenated.front())) != 0
        && title_cased(hyphenated.back());
    return inner_upper || acronym || alphanumeric
        || technical_punctuation || connector_name;
}

std::string clean_token(std::string value) {
    while (!value.empty()
           && std::string_view(".,/\\-_").find(value.back()) != std::string_view::npos) {
        value.pop_back();
    }
    return value;
}

std::string clean_term(std::string_view term) {
    const auto trimmed = detail::trim_unicode_whitespace(term);
    auto parts = detail::split_whitespace(trimmed);
    if (parts.empty()) {
        return {};
    }
    parts.back() = clean_token(std::move(parts.back()));
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += part;
    }
    return result;
}

bool is_safe(std::string_view term) {
    const auto length = detail::codepoint_count(term);
    if (length < 2 || length > 80 || term.find('@') != std::string_view::npos
        || lowercase(term).find("://") != std::string::npos) {
        return false;
    }
    bool has_letter = false;
    std::size_t digit_count = 0;
    for (const auto& point : detail::decode_utf8(term)) {
        has_letter = has_letter || detail::is_letter(point.value);
        if (detail::is_number(point.value)) {
            ++digit_count;
        }
    }
    if (!has_letter) {
        return false;
    }
    return !(length > 24 && term.find(' ') == std::string_view::npos && digit_count > 4);
}

std::string sanitize_sensitive(std::string text) {
    // URLs and address-like strings are removed as complete non-whitespace
    // runs. Replacing bytes with spaces also prevents fragments from being
    // picked up by the term tokenizer.
    const auto points = detail::decode_utf8(text);
    std::size_t index = 0;
    while (index < points.size()) {
        while (index < points.size() && detail::is_whitespace(points[index].value)) {
            ++index;
        }
        if (index == points.size()) {
            break;
        }
        const std::size_t first = index;
        while (index < points.size() && !detail::is_whitespace(points[index].value)) {
            ++index;
        }
        const std::size_t start = points[first].start;
        const std::size_t end = points[index - 1].end;
        const auto run = std::string_view(text).substr(start, end - start);
        const auto folded = lowercase(run);
        const auto at = run.find('@');
        const bool address = at != std::string_view::npos && at > 0;
        const bool url = folded.find("http://") != std::string::npos
            || folded.find("https://") != std::string::npos;
        if (address || url) {
            std::fill(text.begin() + static_cast<std::ptrdiff_t>(start),
                      text.begin() + static_cast<std::ptrdiff_t>(end), ' ');
        }
    }

    // Redact long ASCII token/hash runs even when punctuation surrounds them.
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto byte = static_cast<unsigned char>(text[cursor]);
        const bool token_byte = (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
            || byte == '_' || byte == '-';
        if (!token_byte) {
            ++cursor;
            continue;
        }
        const std::size_t start = cursor;
        while (cursor < text.size()) {
            const auto current = static_cast<unsigned char>(text[cursor]);
            if (!((current >= 'A' && current <= 'Z')
                  || (current >= 'a' && current <= 'z')
                  || (current >= '0' && current <= '9')
                  || current == '_' || current == '-')) {
                break;
            }
            ++cursor;
        }
        if (cursor - start >= 24) {
            std::fill(text.begin() + static_cast<std::ptrdiff_t>(start),
                      text.begin() + static_cast<std::ptrdiff_t>(cursor), ' ');
        }
    }
    return text;
}

struct RawToken {
    std::string raw;
    std::string cleaned;
    std::size_t start{0};
    std::size_t end{0};
};

std::vector<RawToken> tokenize_screen_text(const std::string& text) {
    const auto points = detail::decode_utf8(text);
    std::vector<RawToken> result;
    std::size_t index = 0;
    while (index < points.size()) {
        if (!detail::is_letter(points[index].value)
            && !detail::is_number(points[index].value)) {
            ++index;
            continue;
        }
        const std::size_t first = index;
        ++index;
        while (index < points.size()) {
            const auto value = points[index].value;
            const bool allowed_ascii = value <= 0x7FU
                && std::string_view("._+#/-\\").find(static_cast<char>(value))
                    != std::string_view::npos;
            if (!detail::is_letter(value) && !detail::is_number(value) && !allowed_ascii) {
                break;
            }
            ++index;
        }
        const std::size_t start = points[first].start;
        const std::size_t end = points[index - 1].end;
        auto raw = text.substr(start, end - start);
        result.push_back({raw, clean_token(raw), start, end});
    }
    return result;
}

std::size_t normalized_length(std::string_view value) {
    return detail::codepoint_count(ScreenTermExtractor::normalized(std::string(value)));
}

double similarity(std::string_view lhs, std::string_view rhs) {
    const auto lhs_points = detail::decode_utf8(lhs);
    const auto rhs_points = detail::decode_utf8(rhs);
    if (lhs_points.empty() || rhs_points.empty()) {
        return 0.0;
    }
    std::vector<std::size_t> previous(rhs_points.size() + 1);
    for (std::size_t index = 0; index <= rhs_points.size(); ++index) {
        previous[index] = index;
    }
    for (std::size_t lhs_index = 0; lhs_index < lhs_points.size(); ++lhs_index) {
        std::vector<std::size_t> current(rhs_points.size() + 1);
        current[0] = lhs_index + 1;
        for (std::size_t rhs_index = 0; rhs_index < rhs_points.size(); ++rhs_index) {
            current[rhs_index + 1] = std::min({
                current[rhs_index] + 1,
                previous[rhs_index + 1] + 1,
                previous[rhs_index]
                    + (lhs_points[lhs_index].value == rhs_points[rhs_index].value ? 0U : 1U),
            });
        }
        previous = std::move(current);
    }
    return 1.0 - static_cast<double>(previous.back())
        / static_cast<double>(std::max(lhs_points.size(), rhs_points.size()));
}

std::string spoken_form(std::string_view canonical) {
    std::string result;
    for (const auto& point : detail::decode_utf8(canonical)) {
        switch (point.value) {
        case U'/': result += " slash "; break;
        case U'\\': result += " backslash "; break;
        case U'.': result += " dot "; break;
        case U'_': result += " underscore "; break;
        default:
            result.append(canonical.substr(point.start, point.end - point.start));
            break;
        }
    }
    return result;
}

bool is_structured_spoken_match(std::string_view canonical, std::string_view heard) {
    if (!contains_ascii(canonical, "/\\._")) {
        return false;
    }
    const auto lower_heard = lowercase(heard);
    static constexpr std::string_view markers[] = {
        "slash", "backslash", "dot", "underscore",
    };
    if (!std::any_of(std::begin(markers), std::end(markers), [&](auto marker) {
            return lower_heard.find(marker) != std::string::npos;
        })) {
        return false;
    }

    std::size_t separator = canonical.find_last_of("/\\");
    const auto final_component = separator == std::string_view::npos
        ? canonical : canonical.substr(separator + 1);
    const auto final_key = ScreenTermExtractor::normalized(spoken_form(final_component));
    return !final_key.empty()
        && ScreenTermExtractor::normalized(lower_heard).find(final_key) != std::string::npos;
}

struct WordSpan {
    std::size_t start{0};
    std::size_t end{0};
};

std::vector<WordSpan> word_spans(const std::string& text) {
    const auto points = detail::decode_utf8(text);
    std::vector<WordSpan> result;
    std::size_t index = 0;
    while (index < points.size()) {
        if (!detail::is_letter(points[index].value)
            && !detail::is_number(points[index].value)) {
            ++index;
            continue;
        }
        const std::size_t first = index;
        while (index < points.size()
               && (detail::is_letter(points[index].value)
                   || detail::is_number(points[index].value))) {
            ++index;
        }
        if (index + 1 < points.size()
            && (points[index].value == U'\'' || points[index].value == 0x2019U)
            && (detail::is_letter(points[index + 1].value)
                || detail::is_number(points[index + 1].value))) {
            ++index;
            while (index < points.size()
                   && (detail::is_letter(points[index].value)
                       || detail::is_number(points[index].value))) {
                ++index;
            }
        }
        result.push_back({points[first].start, points[index - 1].end});
    }
    return result;
}

std::vector<std::string> split_path(std::string_view value) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= value.size(); ++index) {
        if (index == value.size() || value[index] == '/' || value[index] == '\\') {
            if (index > start) {
                result.emplace_back(value.substr(start, index - start));
            }
            start = index + 1;
        }
    }
    return result;
}

std::string replace_spaced_marker(
    const std::string& input,
    std::string_view marker,
    std::string_view replacement) {
    const auto points = detail::decode_utf8(input);
    std::string output;
    std::size_t byte_cursor = 0;
    std::size_t index = 0;
    while (index < points.size()) {
        if (!detail::is_whitespace(points[index].value)) {
            ++index;
            continue;
        }
        const std::size_t whitespace_start = index;
        while (index < points.size() && detail::is_whitespace(points[index].value)) {
            ++index;
        }
        const std::size_t word_start = index;
        while (index < points.size() && detail::is_letter(points[index].value)) {
            ++index;
        }
        if (word_start == index || index >= points.size()
            || !detail::is_whitespace(points[index].value)) {
            index = whitespace_start + 1;
            continue;
        }
        const auto word = input.substr(
            points[word_start].start,
            points[index - 1].end - points[word_start].start);
        if (lowercase(word) != marker) {
            index = whitespace_start + 1;
            continue;
        }
        while (index < points.size() && detail::is_whitespace(points[index].value)) {
            ++index;
        }
        const std::size_t start_byte = points[whitespace_start].start;
        const std::size_t end_byte = index == points.size() ? input.size() : points[index].start;
        output.append(input, byte_cursor, start_byte - byte_cursor);
        output += replacement;
        byte_cursor = end_byte;
    }
    output.append(input, byte_cursor, input.size() - byte_cursor);
    return output;
}

bool has_technical_anchor(const std::vector<std::string>& anchors) {
    return std::any_of(anchors.begin(), anchors.end(), [](const auto& anchor) {
        if (contains_ascii(anchor, "._+#/\\-")) {
            return true;
        }
        const auto points = detail::decode_utf8(anchor);
        return points.size() > 1
            && std::any_of(points.begin() + 1, points.end(), [](const auto& point) {
                return detail::is_upper(point.value);
            });
    });
}

std::string normalize_structured_separators(
    std::string text,
    const std::vector<std::string>& anchors) {
    if (!has_technical_anchor(anchors)) {
        return text;
    }
    text = replace_spaced_marker(text, "backslash", "\\");
    text = replace_spaced_marker(text, "slash", "/");
    text = replace_spaced_marker(text, "dot", ".");
    return replace_spaced_marker(text, "underscore", "_");
}

}  // namespace

std::string ScreenTermExtractor::normalized(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const auto& point : detail::decode_utf8(value)) {
        if (detail::is_combining_mark(point.value)) {
            continue;
        }
        if (detail::is_letter(point.value)) {
            result += detail::encode_utf8(detail::latin_base_letter(point.value));
        } else if (detail::is_number(point.value)) {
            result.append(value, point.start, point.end - point.start);
        }
    }
    return result;
}

bool ScreenTermExtractor::is_persistent_candidate(const std::string& term) {
    const auto trimmed = detail::trim_unicode_whitespace(term);
    const auto cleaned = clean_term(term);
    if (cleaned != trimmed || !is_safe(cleaned)) {
        return false;
    }
    const auto tokens = detail::split_whitespace(cleaned);
    if (tokens.size() >= 2) {
        const auto significant = std::count_if(tokens.begin(), tokens.end(), [](const auto& token) {
            return !is_common(token) && (title_cased(token) || distinctive(token));
        });
        return significant >= 2 || contains_ascii(cleaned, "._+#/\\");
    }
    return tokens.size() == 1 && distinctive(tokens.front());
}

bool ScreenTermExtractor::is_correctable_learned_candidate(
    const std::string& term) {
    const auto trimmed = detail::trim_unicode_whitespace(term);
    const auto cleaned = clean_term(term);
    if (cleaned != trimmed || !is_safe(cleaned)) {
        return false;
    }
    if (is_persistent_candidate(cleaned)) {
        return true;
    }
    const auto tokens = detail::split_whitespace(cleaned);
    return tokens.size() == 1 && title_cased(tokens.front())
        && !is_common(tokens.front());
}

std::vector<std::string> ScreenTermExtractor::extract(
    const std::vector<std::string>& visible_strings,
    std::size_t limit) {
    struct Ranked {
        std::string term;
        int score{0};
    };
    std::unordered_map<std::string, Ranked> best;

    auto offer = [&](const std::string& term, int score) {
        const auto cleaned = clean_term(term);
        if (!is_safe(cleaned)) {
            return;
        }
        const auto key = normalized(cleaned);
        if (key.empty()) {
            return;
        }
        const auto existing = best.find(key);
        if (existing == best.end() || existing->second.score < score) {
            best[key] = {cleaned, score};
        }
    };

    const std::size_t input_limit = std::min<std::size_t>(400, visible_strings.size());
    for (std::size_t input_index = 0; input_index < input_limit; ++input_index) {
        const auto sanitized = sanitize_sensitive(visible_strings[input_index]);
        const auto tokens = tokenize_screen_text(sanitized);
        for (const auto& token : tokens) {
            if (token.cleaned.empty()) {
                continue;
            }
            if (distinctive(token.cleaned)) {
                offer(token.cleaned, 4);
            } else if (title_cased(token.cleaned) && !is_common(token.cleaned)) {
                offer(token.cleaned, 2);
            }
            if (token.cleaned.find('/') != std::string::npos) {
                const auto components = split_on(token.cleaned, '/');
                for (const auto& component : components) {
                    if (distinctive(component)) {
                        offer(component, 4);
                    }
                }
                if (components.size() >= 2) {
                    offer(components[components.size() - 2] + "/" + components.back(), 7);
                }
                if (components.size() >= 3) {
                    offer(components[components.size() - 3] + "/"
                        + components[components.size() - 2] + "/" + components.back(), 6);
                }
            }
        }

        std::size_t index = 0;
        while (index < tokens.size()) {
            if (!title_cased(tokens[index].cleaned) && !distinctive(tokens[index].cleaned)) {
                ++index;
                continue;
            }
            std::size_t end = index;
            while (end + 1 < tokens.size() && end - index < 3) {
                const auto& next = tokens[end + 1].cleaned;
                const auto gap = std::string_view(sanitized).substr(
                    tokens[end].end,
                    tokens[end + 1].start - tokens[end].end);
                const auto gap_points = detail::decode_utf8(gap);
                const bool gap_is_whitespace = std::all_of(
                    gap_points.begin(), gap_points.end(), [](const auto& point) {
                        return detail::is_whitespace(point.value);
                    });
                if (!gap_is_whitespace || tokens[end].raw != tokens[end].cleaned) {
                    break;
                }
                if (title_cased(next) || distinctive(next)) {
                    ++end;
                } else if (name_connectors().count(lowercase(next)) != 0
                           && end + 2 < tokens.size()
                           && (title_cased(tokens[end + 2].cleaned)
                               || distinctive(tokens[end + 2].cleaned))) {
                    end += 2;
                } else {
                    break;
                }
            }
            if (end > index) {
                for (std::size_t lower = index; lower < end; ++lower) {
                    if (is_common(tokens[lower].cleaned)) {
                        continue;
                    }
                    std::string phrase = tokens[lower].cleaned;
                    for (std::size_t upper = lower + 1; upper <= end; ++upper) {
                        phrase += " " + tokens[upper].cleaned;
                        offer(phrase, 3 + static_cast<int>(upper - lower));
                    }
                }
            }
            index = end + 1;
        }
    }

    std::vector<Ranked> ranked;
    ranked.reserve(best.size());
    for (auto& entry : best) {
        ranked.push_back(std::move(entry.second));
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& lhs, const Ranked& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        const auto lhs_length = detail::codepoint_count(lhs.term);
        const auto rhs_length = detail::codepoint_count(rhs.term);
        if (lhs_length != rhs_length) {
            return lhs_length > rhs_length;
        }
        return lhs.term < rhs.term;
    });
    std::vector<std::string> result;
    const auto result_count = std::min(limit, ranked.size());
    result.reserve(result_count);
    for (std::size_t index = 0; index < result_count; ++index) {
        result.push_back(std::move(ranked[index].term));
    }
    return result;
}

TerminologyCorrectionResult TerminologyCorrector::correct(
    const std::string& text,
    const std::vector<std::string>& screen_terms,
    const std::vector<LearnedTerm>& learned_terms,
    const std::vector<std::string>& protected_terms) {
    const auto words = word_spans(text);
    if (words.empty()) {
        return {text, {}};
    }

    std::unordered_set<std::string> protected_keys;
    for (const auto& term : protected_terms) {
        protected_keys.insert(ScreenTermExtractor::normalized(term));
    }

    struct Candidate {
        std::string canonical;
        std::vector<std::string> aliases;
        TerminologySource source{TerminologySource::screen};
    };
    std::vector<Candidate> screen_candidates;
    for (const auto& term : screen_terms) {
        if (ScreenTermExtractor::is_persistent_candidate(term)) {
            screen_candidates.push_back({term, {term}, TerminologySource::screen});
        }
    }
    std::vector<std::string> screen_keys;
    screen_keys.reserve(screen_candidates.size());
    for (const auto& candidate : screen_candidates) {
        screen_keys.push_back(ScreenTermExtractor::normalized(candidate.canonical));
    }

    std::vector<Candidate> candidates = screen_candidates;
    for (const auto& learned : learned_terms) {
        if (!ScreenTermExtractor::is_correctable_learned_candidate(
                learned.canonical)) {
            continue;
        }
        const auto key = ScreenTermExtractor::normalized(learned.canonical);
        const bool conflicts_with_screen = std::any_of(
            screen_keys.begin(), screen_keys.end(), [&](const auto& visible) {
                return key != visible && std::min(normalized_length(key), normalized_length(visible)) >= 5
                    && similarity(key, visible) >= 0.78;
            });
        if (conflicts_with_screen) {
            continue;
        }
        std::vector<std::string> aliases{learned.canonical};
        for (const auto& alias : learned.aliases) {
            const auto alias_key = ScreenTermExtractor::normalized(alias);
            if (alias_key.empty()) {
                continue;
            }
            const bool close = std::min(
                    normalized_length(alias_key), normalized_length(key)) >= 6
                && similarity(alias_key, key) >= 0.72;
            if (alias_key == key || close
                || is_structured_spoken_match(learned.canonical, alias)) {
                aliases.push_back(alias);
            }
        }
        candidates.push_back({learned.canonical, std::move(aliases), TerminologySource::learned});
    }

    std::unordered_set<std::string> components_covered_by_full_path;
    for (const auto& candidate : candidates) {
        if (candidate.canonical.find('/') == std::string::npos
            && candidate.canonical.find('\\') == std::string::npos) {
            continue;
        }
        for (const auto& component : split_path(candidate.canonical)) {
            components_covered_by_full_path.insert(ScreenTermExtractor::normalized(component));
        }
    }

    struct Proposal {
        std::size_t start{0};
        std::size_t end{0};
        std::string canonical;
        std::string heard;
        double confidence{0.0};
        TerminologySource source{TerminologySource::screen};
        std::size_t order{0};
    };
    std::vector<Proposal> proposals;
    std::size_t proposal_order = 0;
    for (const auto& candidate : candidates) {
        const auto canonical_key = ScreenTermExtractor::normalized(candidate.canonical);
        if (normalized_length(canonical_key) < 2
            || protected_keys.count(canonical_key) != 0) {
            continue;
        }
        std::vector<std::string> comparison_aliases;
        comparison_aliases.reserve(candidate.aliases.size() * 2);
        std::size_t canonical_word_count = 1;
        for (const auto& alias : candidate.aliases) {
            comparison_aliases.push_back(alias);
            comparison_aliases.push_back(spoken_form(alias));
            canonical_word_count = std::max(
                canonical_word_count,
                detail::split_whitespace(alias).size());
            canonical_word_count = std::max(
                canonical_word_count,
                detail::split_whitespace(spoken_form(alias)).size());
        }
        const std::size_t min_words = std::max<std::size_t>(1, canonical_word_count - 1);
        const std::size_t max_words = std::min<std::size_t>(8, canonical_word_count + 2);
        if (min_words > max_words) {
            continue;
        }
        for (std::size_t start = 0; start < words.size(); ++start) {
            for (std::size_t count = min_words;
                 count <= max_words && start + count <= words.size(); ++count) {
                const std::size_t range_start = words[start].start;
                const std::size_t range_end = words[start + count - 1].end;
                const auto heard = text.substr(range_start, range_end - range_start);
                const auto heard_key = ScreenTermExtractor::normalized(heard);
                if (heard_key.empty() || protected_keys.count(heard_key) != 0
                    || heard == candidate.canonical) {
                    continue;
                }

                const auto heard_words = detail::split_whitespace(lowercase(heard));
                const bool mentions_separator = std::find(heard_words.begin(), heard_words.end(), "slash")
                        != heard_words.end()
                    || std::find(heard_words.begin(), heard_words.end(), "backslash")
                        != heard_words.end();
                bool preceded_by_separator = false;
                if (start > 0) {
                    const auto previous = lowercase(text.substr(
                        words[start - 1].start,
                        words[start - 1].end - words[start - 1].start));
                    preceded_by_separator = previous == "slash" || previous == "backslash";
                }
                if (components_covered_by_full_path.count(canonical_key) != 0
                    && (mentions_separator || preceded_by_separator)) {
                    continue;
                }

                double confidence = 0.0;
                for (const auto& alias : comparison_aliases) {
                    const auto alias_key = ScreenTermExtractor::normalized(alias);
                    if (heard_key == alias_key) {
                        confidence = 1.0;
                        break;
                    }
                    if (std::min(normalized_length(heard_key), normalized_length(alias_key)) < 6) {
                        continue;
                    }
                    confidence = std::max(confidence, similarity(heard_key, alias_key));
                }
                const bool structured = is_structured_spoken_match(candidate.canonical, heard);
                const double threshold = structured ? 0.72
                    : (candidate.source == TerminologySource::screen ? 0.88 : 0.92);
                if (confidence >= threshold) {
                    proposals.push_back({
                        range_start,
                        range_end,
                        candidate.canonical,
                        heard,
                        confidence,
                        candidate.source,
                        proposal_order++,
                    });
                }
            }
        }
    }

    std::sort(proposals.begin(), proposals.end(), [](const Proposal& lhs, const Proposal& rhs) {
        if (lhs.confidence != rhs.confidence) {
            return lhs.confidence > rhs.confidence;
        }
        if (lhs.end - lhs.start != rhs.end - rhs.start) {
            return lhs.end - lhs.start > rhs.end - rhs.start;
        }
        if (lhs.source != rhs.source) {
            return lhs.source == TerminologySource::screen;
        }
        return lhs.order < rhs.order;
    });
    std::vector<Proposal> accepted;
    for (const auto& proposal : proposals) {
        const bool overlaps = std::any_of(accepted.begin(), accepted.end(), [&](const auto& other) {
            return proposal.start < other.end && other.start < proposal.end;
        });
        if (!overlaps) {
            accepted.push_back(proposal);
        }
    }
    std::sort(accepted.begin(), accepted.end(), [](const Proposal& lhs, const Proposal& rhs) {
        return lhs.start < rhs.start;
    });

    std::string output;
    std::size_t cursor = 0;
    std::vector<std::string> anchors;
    std::vector<TerminologyMatch> matches;
    for (const auto& replacement : accepted) {
        output.append(text, cursor, replacement.start - cursor);
        output += replacement.canonical;
        cursor = replacement.end;
        anchors.push_back(replacement.canonical);
        matches.push_back({
            replacement.heard,
            replacement.canonical,
            replacement.confidence,
            replacement.source,
        });
    }
    output.append(text, cursor, text.size() - cursor);
    output = normalize_structured_separators(std::move(output), anchors);
    return {std::move(output), std::move(matches)};
}

}  // namespace localflow::core
