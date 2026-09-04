#pragma once

#include <string>
#include <vector>

namespace localflow::core {

struct ReplacementRule {
    std::string spoken;
    std::string written;

    friend bool operator==(const ReplacementRule& lhs, const ReplacementRule& rhs) {
        return lhs.spoken == rhs.spoken && lhs.written == rhs.written;
    }
};

struct PersonalDictionary {
    std::vector<ReplacementRule> rules;
    bool spoken_punctuation_enabled{false};

    friend bool operator==(const PersonalDictionary& lhs, const PersonalDictionary& rhs) {
        return lhs.rules == rhs.rules
            && lhs.spoken_punctuation_enabled == rhs.spoken_punctuation_enabled;
    }
};

class ReplacementEngine {
public:
    explicit ReplacementEngine(PersonalDictionary dictionary = {});

    [[nodiscard]] const PersonalDictionary& dictionary() const noexcept;
    [[nodiscard]] std::string apply(const std::string& text) const;

private:
    struct CompiledRule {
        std::string spoken;
        std::string written;
        std::size_t spoken_length{0};
        std::size_t priority{0};
    };

    PersonalDictionary dictionary_;
    std::vector<CompiledRule> compiled_;
};

}  // namespace localflow::core

