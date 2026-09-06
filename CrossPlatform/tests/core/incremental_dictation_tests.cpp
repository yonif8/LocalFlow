#include "localflow/core/incremental_dictation.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace localflow::core;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Test-only splitter. Production uses Qt's Unicode sentence boundaries.
auto split(const std::string& text) {
    const auto boundary = text.rfind(". ");
    return boundary == std::string::npos
        ? std::make_pair(std::string{}, text)
        : std::make_pair(text.substr(0, boundary + 1), text.substr(boundary + 2));
}

int main() {
    try {
        const Utterance voice{std::vector<float>(16000, 0.1f), 16000};
        const Utterance silence{std::vector<float>(16000, 0), 16000};
        const auto identity = [](const std::string& text) { return text; };
        const auto active = [] { return false; };
        {
            IncrementalDictation stream;
            std::vector<std::string> text{"First sentence. Second sentence.", "Third sentence.", "Final words."};
            std::vector<std::string> polished;
            std::size_t index = 0;
            const auto asr = [&](const Utterance&) { return text.at(index++); };
            const auto polish = [&](const std::string& input) { polished.push_back(input); return input; };
            stream.append(voice, asr, polish, split, active);
            stream.append(voice, asr, polish, split, active);
            check(stream.finish(voice, asr, polish, active) ==
                "First sentence. Second sentence. Third sentence. Final words.", "ordering or words lost");
            check(polished == std::vector<std::string>{"First sentence.", "Second sentence.", "Third sentence. Final words."}, "sentence carry incorrect");
        }
        {
            IncrementalDictation stream;
            unsigned calls = 0;
            const auto asr = [&](const Utterance&) { ++calls; return "Keep the final words."; };
            stream.append(voice, asr, identity, split, active);
            check(stream.finish(silence, asr, identity, active) == "Keep the final words.", "quiet tail dropped carry");
            check(calls == 1, "quiet release ran ASR");
        }
        {
            IncrementalDictation stream;
            const auto asr = [](const Utterance&) { return "Short message."; };
            check(stream.finish(voice, asr, identity, active) == "Short message.", "short path changed");
            bool rejected = false;
            try { (void)stream.finish(voice, asr, identity, active); } catch (...) { rejected = true; }
            check(rejected, "closed session reused");
        }
        {
            IncrementalDictation stream;
            const auto broken = [](const Utterance&) -> std::string { throw std::runtime_error("ASR failed"); };
            bool rejected = false;
            try { stream.append(voice, broken, identity, split, active); } catch (...) { rejected = true; }
            check(rejected, "failed ASR suppressed");
            rejected = false;
            try { (void)stream.finish(silence, broken, identity, active); } catch (...) { rejected = true; }
            check(rejected, "partial success after failure");
        }
        {
            IncrementalDictation stream;
            unsigned calls = 0;
            bool cancelled = true;
            const auto asr = [&](const Utterance&) { ++calls; return "Cancelled words."; };
            bool rejected = false;
            try { stream.append(voice, asr, identity, split, [&] { return cancelled; }); } catch (...) { rejected = true; }
            check(rejected && calls == 0, "cancelled session started ASR");
            IncrementalDictation fresh;
            check(fresh.finish(voice, [](const Utterance&) { return "New message."; }, identity, active) == "New message.", "cancelled words leaked");
        }
        {
            DictationSegmentBudget budget;
            const auto initial = budget.pause_search_seconds();
            check(initial > 27 && initial < 28, "seed differs from macOS calibration");
            budget.observe(30, 0.2);
            check(budget.pause_search_seconds() == initial, "fast chunk loosened budget");
            budget.observe(30, 3);
            check(budget.pause_search_seconds() < initial, "slow chunk did not tighten budget");
            check(DictationSegmentBudget::has_quiet_tail(silence.samples, 16000), "quiet tail rejected");
            check(!DictationSegmentBudget::has_quiet_tail(voice.samples, 16000), "speech cut by stale meter");
            check(!DictationSegmentBudget::has_quiet_tail({}, 16000), "empty snapshot accepted");
            auto resumed = silence.samples;
            resumed.insert(resumed.end(), 1600, 0.1f);
            check(!DictationSegmentBudget::has_quiet_tail(resumed, 16000), "resumed speech cut");
        }
        std::cout << "6 incremental dictation scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
