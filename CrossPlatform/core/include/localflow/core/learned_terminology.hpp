#pragma once

#include "localflow/core/terminology.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace localflow::core {

class LearnedTerminologyBank {
public:
    static constexpr std::size_t max_terms = 500;
    static constexpr std::size_t max_aliases_per_term = 10;

    explicit LearnedTerminologyBank(std::vector<LearnedTerm> decoded_terms = {});

    [[nodiscard]] const std::vector<LearnedTerm>& terms() const noexcept;

    // Call this at the persistence boundary. Invalid legacy entries are
    // removed, term count is capped, and only the newest aliases survive.
    [[nodiscard]] static std::vector<LearnedTerm> sanitized(
        const std::vector<LearnedTerm>& decoded_terms);

    void learn(
        const std::vector<TerminologyMatch>& matches,
        std::optional<std::string> source_app_id = std::nullopt,
        std::int64_t now_ms = current_time_ms());

    void replace(std::vector<LearnedTerm> decoded_terms);
    void clear() noexcept;

    [[nodiscard]] static std::int64_t current_time_ms() noexcept;

private:
    std::vector<LearnedTerm> terms_;
};

}  // namespace localflow::core

