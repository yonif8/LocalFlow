#include "localflow/core/contracts.hpp"
#include "localflow/core/learned_terminology.hpp"
#include "localflow/core/replacements.hpp"
#include "localflow/core/terminology.hpp"
#include "localflow/core/text_chunker.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lf = localflow::core;

namespace {

class TestRunner {
public:
    template <typename Function>
    void run(const std::string& name, Function&& function) {
        ++tests_;
        try {
            function();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures_;
            std::cerr << "[fail] " << name << ": unknown exception\n";
        }
    }

    [[nodiscard]] int exit_code() const {
        std::cout << (tests_ - failures_) << '/' << tests_ << " tests passed\n";
        return failures_ == 0 ? 0 : 1;
    }

private:
    int tests_{0};
    int failures_{0};
};

void fail(const char* expression, const char* file, int line) {
    std::ostringstream message;
    message << file << ':' << line << ": expected " << expression;
    throw std::runtime_error(message.str());
}

#define LF_CHECK(expression) \
    do { if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (false)

bool contains(const std::vector<std::string>& values, const std::string& expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        result += value;
    }
    return result;
}

lf::ReplacementEngine replacement_engine(
    std::initializer_list<std::pair<std::string, std::string>> rules,
    bool punctuation = false) {
    lf::PersonalDictionary dictionary;
    dictionary.spoken_punctuation_enabled = punctuation;
    for (const auto& [spoken, written] : rules) {
        dictionary.rules.push_back({spoken, written});
    }
    return lf::ReplacementEngine(std::move(dictionary));
}

lf::LearnedTerm learned(std::string canonical, std::vector<std::string> aliases = {}) {
    lf::LearnedTerm result;
    result.id = canonical;
    result.canonical = std::move(canonical);
    result.aliases = std::move(aliases);
    result.created_at_ms = 1;
    result.last_used_at_ms = 1;
    return result;
}

void contracts_tests(TestRunner& runner) {
    runner.run("utterance duration", [] {
        lf::Utterance utterance{{0.0F, 0.1F, -0.1F, 0.0F}, 2.0};
        LF_CHECK(std::abs(utterance.duration_seconds() - 2.0) < 0.000001);
        utterance.sample_rate_hz = 0.0;
        LF_CHECK(utterance.duration_seconds() == 0.0);
    });

    runner.run("capture event factories", [] {
        const auto began = lf::CaptureEvent::began();
        LF_CHECK(began.kind == lf::CaptureEventKind::began);
        const auto ended = lf::CaptureEvent::ended({{1.0F}, 16'000.0});
        LF_CHECK(ended.kind == lf::CaptureEventKind::ended);
        LF_CHECK(ended.utterance.has_value());
        const auto level = lf::CaptureEvent::level(0.25F);
        LF_CHECK(level.kind == lf::CaptureEventKind::level);
        LF_CHECK(level.input_level == 0.25F);
    });
}

void replacement_tests(TestRunner& runner) {
    runner.run("dictionary basic, case-insensitive, and whole-word matching", [] {
        const auto engine = replacement_engine({{"echidna cams", "EchidnaCams"}, {"cat", "CAT"}});
        LF_CHECK(engine.apply("check out echidna cams today")
            == "check out EchidnaCams today");
        LF_CHECK(engine.apply("ECHIDNA CAMS is live") == "EchidnaCams is live");
        LF_CHECK(engine.apply("the cat sat; concatenate category")
            == "the CAT sat; concatenate category");
    });

    runner.run("dictionary adapts author-lowercase output casing", [] {
        const auto engine = replacement_engine({{"localflow", "localflow app"}});
        LF_CHECK(engine.apply("Localflow is great") == "Localflow app is great");
        LF_CHECK(engine.apply("LOCALFLOW is great") == "LOCALFLOW APP is great");
        LF_CHECK(engine.apply("use localflow daily") == "use localflow app daily");
    });

    runner.run("author casing is verbatim", [] {
        const auto engine = replacement_engine({{"i b m", "IBM"}, {"echidna cams", "EchidnaCams"}});
        LF_CHECK(engine.apply("I B M and ECHIDNA CAMS") == "IBM and EchidnaCams");
    });

    runner.run("longest overlapping rule wins", [] {
        const auto engine = replacement_engine({
            {"new york", "New York"},
            {"new york times", "The New York Times"},
        });
        LF_CHECK(engine.apply("read the new york times daily")
            == "read the The New York Times daily");
        LF_CHECK(engine.apply("i love new york") == "i love New York");
    });

    runner.run("rules tolerate flexible Unicode whitespace", [] {
        const auto engine = replacement_engine({{"echidna cams", "EchidnaCams"}});
        LF_CHECK(engine.apply("echidna\t \ncams") == "EchidnaCams");
    });

    runner.run("replacement is single pass and non-cascading", [] {
        const auto engine = replacement_engine({{"alpha", "beta"}, {"beta", "gamma"}});
        LF_CHECK(engine.apply("alpha beta") == "beta gamma");
    });

    runner.run("ordinary dictionaries are idempotent", [] {
        const auto engine = replacement_engine({
            {"echidna cams", "EchidnaCams"},
            {"i b m", "IBM"},
            {"localflow", "LocalFlow"},
        });
        const auto once = engine.apply("Localflow syncs echidna cams footage to i b m cloud");
        LF_CHECK(engine.apply(once) == once);
    });

    runner.run("spoken punctuation is opt-in", [] {
        const auto disabled = replacement_engine({});
        LF_CHECK(disabled.apply("hello comma world period") == "hello comma world period");
        const auto enabled = replacement_engine({}, true);
        LF_CHECK(enabled.apply("hello comma world period") == "hello, world.");
        LF_CHECK(enabled.apply("really question mark") == "really?");
        LF_CHECK(enabled.apply("first line new line second line") == "first line\nsecond line");
        LF_CHECK(enabled.apply("one new paragraph two") == "one\n\ntwo");
    });
}

void chunker_tests(TestRunner& runner) {
    runner.run("chunker round trips multilingual and emoji text", [] {
        const std::vector<std::string> samples = {
            "hello world this is a longer piece of dictated text!",
            "emoji 👍🏽 family 👨‍👩‍👧‍👦 flags 🇦🇺🇯🇵 mixed with ASCII",
            "accents: cafe\u0301 naïve résumé — and “smart quotes”",
            "newlines\nand\ttabs survive",
            "日本語のテキストと한국어 텍스트 and English",
        };
        for (const auto& sample : samples) {
            for (const std::size_t size : {1U, 3U, 7U, 20U, 64U}) {
                const auto chunks = lf::chunk_text_utf16(sample, size);
                LF_CHECK(join(chunks) == sample);
                LF_CHECK(std::all_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
                    return !chunk.empty();
                }));
            }
        }
    });

    runner.run("chunker never splits emoji graphemes", [] {
        const std::string family = "👨‍👩‍👧‍👦";
        const auto chunks = lf::chunk_text_utf16("abc" + family + "def", 4);
        LF_CHECK(join(chunks) == "abc" + family + "def");
        LF_CHECK(contains(chunks, family));
        for (const auto& chunk : chunks) {
            LF_CHECK(lf::utf16_length(chunk) <= 4 || chunk == family);
        }
        LF_CHECK(lf::chunk_text_utf16("𝕏𝕐𝕑", 1)
            == std::vector<std::string>({"𝕏", "𝕐", "𝕑"}));
    });

    runner.run("chunker rejects a zero limit", [] {
        bool threw = false;
        try {
            (void)lf::chunk_text_utf16("hello", 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        LF_CHECK(threw);
    });
}

void extraction_tests(TestRunner& runner) {
    runner.run("extracts technical shapes and proper names", [] {
        const auto terms = lf::ScreenTermExtractor::extract({
            "Configure PostgreSQL in DataGrip",
            "Connect Jane Smith to API v2",
            "Open LocalFlow/Sources/auth_token.swift",
        });
        LF_CHECK(contains(terms, "PostgreSQL"));
        LF_CHECK(contains(terms, "DataGrip"));
        LF_CHECK(contains(terms, "Jane Smith"));
        LF_CHECK(contains(terms, "API"));
        LF_CHECK(contains(terms, "v2"));
    });

    runner.run("extracts useful path suffixes", [] {
        const auto terms = lf::ScreenTermExtractor::extract({
            "/Users/yoni/Projects/LocalFlow/Sources/LFPolish/Terminology.swift",
        });
        LF_CHECK(contains(terms, "LFPolish/Terminology.swift"));
        LF_CHECK(contains(terms, "Terminology.swift"));
    });

    runner.run("extracts names with lowercase connectors", [] {
        const auto terms = lf::ScreenTermExtractor::extract({"Bab el-Mandeb Ship Count"});
        LF_CHECK(contains(terms, "Bab el-Mandeb"));
    });

    runner.run("persistent candidates require strong generic shape", [] {
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("The"));
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("Check"));
        LF_CHECK(lf::ScreenTermExtractor::is_persistent_candidate("PostgreSQL"));
        LF_CHECK(lf::ScreenTermExtractor::is_persistent_candidate("Terminology.swift"));
        LF_CHECK(lf::ScreenTermExtractor::is_persistent_candidate("Jane Smith"));
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("it."));
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("enable."));
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("WITH"));
        LF_CHECK(!lf::ScreenTermExtractor::is_persistent_candidate("Can You"));
    });

    runner.run("extractor excludes secrets, URLs, and addresses", [] {
        const auto terms = lf::ScreenTermExtractor::extract({
            "person@example.com",
            "A94F20B81C7348899918273619283746",
            "https://internal.example.com/SecretProject",
            "Settings Save Cancel",
        });
        LF_CHECK(std::none_of(terms.begin(), terms.end(), [](const auto& term) {
            return term.find('@') != std::string::npos
                || term.find("SecretProject") != std::string::npos
                || term == "A94F20B81C7348899918273619283746"
                || term == "Settings";
        }));
    });

    runner.run("sentence punctuation and emphasis do not become terms", [] {
        const auto terms = lf::ScreenTermExtractor::extract({
            "It works. Search is enabled.",
            "WITH NOT You Can",
        });
        LF_CHECK(!contains(terms, "works."));
        LF_CHECK(!contains(terms, "enabled."));
        LF_CHECK(!contains(terms, "WITH"));
        LF_CHECK(!contains(terms, "NOT"));
        LF_CHECK(!contains(terms, "You Can"));
    });

    runner.run("name phrases cannot cross sentence boundaries", [] {
        const auto terms = lf::ScreenTermExtractor::extract({"Hello World. Native Codex"});
        LF_CHECK(!contains(terms, "World Native"));
        LF_CHECK(contains(terms, "Native Codex"));
    });
}

void correction_tests(TestRunner& runner) {
    runner.run("restores visible casing and boundaries", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "configure postgresql in data grip", {"PostgreSQL", "DataGrip"}, {});
        LF_CHECK(result.text == "configure PostgreSQL in DataGrip");
        LF_CHECK(result.matches.size() == 2);
        LF_CHECK(std::all_of(result.matches.begin(), result.matches.end(), [](const auto& match) {
            return match.source == lf::TerminologySource::screen;
        }));
    });

    runner.run("screen fuzzy correction is conservative", [] {
        LF_CHECK(lf::TerminologyCorrector::correct(
            "open postgressql", {"PostgreSQL"}, {}).text == "open PostgreSQL");
        LF_CHECK(lf::TerminologyCorrector::correct(
            "open postal sequel", {"PostgreSQL"}, {}).text == "open postal sequel");
    });

    runner.run("learned aliases work away from source window", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "use postgressql tomorrow", {}, {learned("PostgreSQL", {"postgressql"})});
        LF_CHECK(result.text == "use PostgreSQL tomorrow");
        LF_CHECK(result.matches.size() == 1);
        LF_CHECK(result.matches.front().source == lf::TerminologySource::learned);
    });

    runner.run("personal dictionary spellings are protected", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "use Postgres tomorrow", {"PostgreSQL"}, {}, {"Postgres"});
        LF_CHECK(result.text == "use Postgres tomorrow");
        LF_CHECK(result.matches.empty());
    });

    runner.run("ordinary visible words never capitalize prose", [] {
        const auto text = "Can you decide what to enable and what to disable?";
        const auto result = lf::TerminologyCorrector::correct(
            text, {"You", "And", "Check"}, {});
        LF_CHECK(result.text == text);
        LF_CHECK(result.matches.empty());
    });

    runner.run("explicitly learned names restore capitalization", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "ask alice tomorrow", {}, {learned("Alice")});
        LF_CHECK(result.text == "ask Alice tomorrow");
        LF_CHECK(result.matches.size() == 1);
    });

    runner.run("learned fuzzy matching does not change inflection", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "the option is enabled", {}, {learned("enable.", {"enable"})});
        LF_CHECK(result.text == "the option is enabled");
        LF_CHECK(result.matches.empty());
    });

    runner.run("recovers a spoken file path", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "L'Philippines slash terminology dot swift.",
            {"LFPolish/Terminology.swift", "Terminology.swift"}, {});
        LF_CHECK(result.text == "LFPolish/Terminology.swift.");
        LF_CHECK(result.matches.size() == 1);
        LF_CHECK(result.matches.front().canonical == "LFPolish/Terminology.swift");
    });

    runner.run("restores separators around individually anchored terms", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "sources slash local flow app slash bug report view dot swift.",
            {"LocalFlowApp", "BugReportView.swift"}, {});
        LF_CHECK(result.text == "sources/LocalFlowApp/BugReportView.swift.");
    });

    runner.run("corrects connector names as a full phrase", [] {
        LF_CHECK(lf::TerminologyCorrector::correct(
            "Bob Elman Deb ship count", {"Bab el-Mandeb"}, {}).text
            == "Bab el-Mandeb ship count");
    });

    runner.run("visible terminology beats conflicting memory", [] {
        const auto result = lf::TerminologyCorrector::correct(
            "deep seek harness", {"DeepSeek Harness"},
            {learned("DeepSea", {"deep sea"})});
        LF_CHECK(result.text == "DeepSeek Harness");
        LF_CHECK(result.matches.size() == 1);
        LF_CHECK(result.matches.front().source == lf::TerminologySource::screen);
    });
}

void learned_bank_tests(TestRunner& runner) {
    runner.run("bank removes legacy pollution and caps aliases", [] {
        auto valid = learned("PostgreSQL");
        for (int index = 0; index < 14; ++index) {
            valid.aliases.push_back("alias" + std::to_string(index));
        }
        lf::LearnedTerminologyBank bank({
            learned("it."), learned("enable."), learned("WITH"),
            learned("Can You"), valid,
        });
        LF_CHECK(bank.terms().size() == 1);
        LF_CHECK(bank.terms().front().canonical == "PostgreSQL");
        LF_CHECK(bank.terms().front().aliases.size() == 10);
        LF_CHECK(bank.terms().front().aliases.front() == "alias4");
    });

    runner.run("bank has a hard 500-term recency cap", [] {
        std::vector<lf::LearnedTerm> terms;
        for (int index = 0; index < 505; ++index) {
            auto term = learned("Product" + std::to_string(index));
            term.last_used_at_ms = index;
            terms.push_back(std::move(term));
        }
        lf::LearnedTerminologyBank bank(std::move(terms));
        LF_CHECK(bank.terms().size() == lf::LearnedTerminologyBank::max_terms);
    });

    runner.run("learning bounds aliases and refreshes successful reuse", [] {
        lf::LearnedTerminologyBank bank;
        for (int index = 0; index < 12; ++index) {
            bank.learn({{
                "data grip " + std::to_string(index),
                "DataGrip",
                1.0,
                lf::TerminologySource::screen,
            }}, "com.example.ide", 100 + index);
        }
        LF_CHECK(bank.terms().size() == 1);
        LF_CHECK(bank.terms().front().aliases.size() == 10);
        LF_CHECK(bank.terms().front().aliases.front() == "data grip 2");
        const auto prior_count = bank.terms().front().use_count;
        bank.learn({{
            "data grip 11", "DataGrip", 1.0, lf::TerminologySource::learned,
        }}, std::nullopt, 999);
        LF_CHECK(bank.terms().front().use_count == prior_count + 1);
        LF_CHECK(bank.terms().front().last_used_at_ms == 999);
    });

    runner.run("low-confidence screen guesses are never learned", [] {
        lf::LearnedTerminologyBank bank;
        bank.learn({{
            "post sequel", "PostgreSQL", 0.70, lf::TerminologySource::screen,
        }}, std::nullopt, 10);
        LF_CHECK(bank.terms().empty());
    });
}

void golden_regressions(TestRunner& runner) {
    runner.run("golden: contaminated screen words cannot alter casual prose", [] {
        const std::string text =
            "Can you just list all the disabled shit? Like, I didn't disable anything. "
            "So let me look at it so I can decide what to enable and what to disable.";
        lf::LearnedTerminologyBank bank({
            learned("it."), learned("enable."), learned("WITH"),
            learned("You"), learned("And"), learned("Can You"),
        });
        const auto result = lf::TerminologyCorrector::correct(
            text, {"You", "And", "Check", "WITH"}, bank.terms());
        LF_CHECK(result.text == text);
        LF_CHECK(result.matches.empty());
    });

    runner.run("golden: normal prose stays intact beside a technical token", [] {
        const std::string text =
            "So I don't understand. Okay, so v2 is the only version. But is it enabled? "
            "Like, if I ask him to have a sub agent, will he run a sub agent? "
            "And how will I see it on Telegram?";
        lf::LearnedTerminologyBank bank({
            learned("it."), learned("enable."), learned("WITH"),
        });
        const auto result = lf::TerminologyCorrector::correct(
            text, {"V2", "You", "And"}, bank.terms());
        LF_CHECK(result.text ==
            "So I don't understand. Okay, so V2 is the only version. But is it enabled? "
            "Like, if I ask him to have a sub agent, will he run a sub agent? "
            "And how will I see it on Telegram?");
        LF_CHECK(result.matches.size() == 1);
        LF_CHECK(result.matches.front().canonical == "V2");
    });
}

}  // namespace

int main() {
    TestRunner runner;
    contracts_tests(runner);
    replacement_tests(runner);
    chunker_tests(runner);
    extraction_tests(runner);
    correction_tests(runner);
    learned_bank_tests(runner);
    golden_regressions(runner);
    return runner.exit_code();
}

