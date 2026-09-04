#include "localflow/core/learned_terminology.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace localflow::core {
namespace {

std::string make_id(std::int64_t now_ms) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto value = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream stream;
    stream << std::hex << now_ms << '-' << value;
    return stream.str();
}

void increment_use_count(LearnedTerm& term) noexcept {
    if (term.use_count != std::numeric_limits<std::uint32_t>::max()) {
        ++term.use_count;
    }
}

}  // namespace

LearnedTerminologyBank::LearnedTerminologyBank(std::vector<LearnedTerm> decoded_terms) {
    replace(std::move(decoded_terms));
}

const std::vector<LearnedTerm>& LearnedTerminologyBank::terms() const noexcept {
    return terms_;
}

std::vector<LearnedTerm> LearnedTerminologyBank::sanitized(
    const std::vector<LearnedTerm>& decoded_terms) {
    std::vector<LearnedTerm> result;
    result.reserve(std::min(max_terms, decoded_terms.size()));
    for (const auto& decoded : decoded_terms) {
        if (result.size() == max_terms) {
            break;
        }
        if (!ScreenTermExtractor::is_persistent_candidate(decoded.canonical)) {
            continue;
        }
        auto term = decoded;
        if (term.aliases.size() > max_aliases_per_term) {
            term.aliases.erase(
                term.aliases.begin(),
                term.aliases.end() - static_cast<std::ptrdiff_t>(max_aliases_per_term));
        }
        result.push_back(std::move(term));
    }
    return result;
}

void LearnedTerminologyBank::learn(
    const std::vector<TerminologyMatch>& matches,
    std::optional<std::string> source_app_id,
    std::int64_t now_ms) {
    const bool has_eligible_match = std::any_of(matches.begin(), matches.end(), [](const auto& match) {
        return match.source == TerminologySource::learned
            || (match.source == TerminologySource::screen
                && match.confidence >= 0.88
                && ScreenTermExtractor::is_persistent_candidate(match.canonical));
    });
    if (!has_eligible_match) {
        return;
    }

    // Successful learned matches refresh recency but cannot add a new alias:
    // by definition that spelling was already in the bank.
    for (const auto& match : matches) {
        if (match.source != TerminologySource::learned) {
            continue;
        }
        const auto key = ScreenTermExtractor::normalized(match.canonical);
        const auto existing = std::find_if(terms_.begin(), terms_.end(), [&](const auto& term) {
            return ScreenTermExtractor::normalized(term.canonical) == key;
        });
        if (existing != terms_.end()) {
            increment_use_count(*existing);
            existing->last_used_at_ms = now_ms;
        }
    }

    for (const auto& match : matches) {
        if (match.source != TerminologySource::screen || match.confidence < 0.88
            || !ScreenTermExtractor::is_persistent_candidate(match.canonical)) {
            continue;
        }
        const auto key = ScreenTermExtractor::normalized(match.canonical);
        const auto existing = std::find_if(terms_.begin(), terms_.end(), [&](const auto& term) {
            return ScreenTermExtractor::normalized(term.canonical) == key;
        });
        if (existing != terms_.end()) {
            const bool canonical_heard = detail::case_insensitive_equal(
                match.heard, match.canonical);
            const bool alias_exists = std::any_of(
                existing->aliases.begin(), existing->aliases.end(), [&](const auto& alias) {
                    return detail::case_insensitive_equal(alias, match.heard);
                });
            if (!canonical_heard && !alias_exists) {
                existing->aliases.push_back(match.heard);
                if (existing->aliases.size() > max_aliases_per_term) {
                    existing->aliases.erase(
                        existing->aliases.begin(),
                        existing->aliases.end()
                            - static_cast<std::ptrdiff_t>(max_aliases_per_term));
                }
            }
            increment_use_count(*existing);
            existing->last_used_at_ms = now_ms;
        } else {
            LearnedTerm term;
            term.id = make_id(now_ms);
            term.canonical = match.canonical;
            if (!detail::case_insensitive_equal(match.heard, match.canonical)) {
                term.aliases = {match.heard};
            }
            term.use_count = 1;
            term.created_at_ms = now_ms;
            term.last_used_at_ms = now_ms;
            term.source_app_id = source_app_id;
            terms_.push_back(std::move(term));
        }
    }

    std::stable_sort(terms_.begin(), terms_.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.last_used_at_ms > rhs.last_used_at_ms;
    });
    if (terms_.size() > max_terms) {
        terms_.resize(max_terms);
    }
}

void LearnedTerminologyBank::replace(std::vector<LearnedTerm> decoded_terms) {
    terms_ = sanitized(decoded_terms);
}

void LearnedTerminologyBank::clear() noexcept {
    terms_.clear();
}

std::int64_t LearnedTerminologyBank::current_time_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace localflow::core

