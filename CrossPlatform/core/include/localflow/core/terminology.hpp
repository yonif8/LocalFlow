#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace localflow::core {

enum class TerminologySource { screen, learned };

struct LearnedTerm {
    std::string id;
    std::string canonical;
    std::vector<std::string> aliases;
    std::uint32_t use_count{1};
    std::int64_t created_at_ms{0};
    std::int64_t last_used_at_ms{0};
    std::optional<std::string> source_app_id;
};

struct TerminologyMatch {
    std::string heard;
    std::string canonical;
    double confidence{0.0};
    TerminologySource source{TerminologySource::screen};
};

struct TerminologyCorrectionResult {
    std::string text;
    std::vector<TerminologyMatch> matches;
};

class ScreenTermExtractor {
public:
    [[nodiscard]] static std::vector<std::string> extract(
        const std::vector<std::string>& visible_strings,
        std::size_t limit = 120);

    [[nodiscard]] static bool is_persistent_candidate(const std::string& term);
    [[nodiscard]] static std::string normalized(const std::string& value);
};

class TerminologyCorrector {
public:
    [[nodiscard]] static TerminologyCorrectionResult correct(
        const std::string& text,
        const std::vector<std::string>& screen_terms,
        const std::vector<LearnedTerm>& learned_terms,
        const std::vector<std::string>& protected_terms = {});
};

}  // namespace localflow::core

