#pragma once

#include "localflow/core/contracts.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace localflow::core {

// Same policy as macOS (see docs/INCREMENTAL_DICTATION.md). Accuracy outranks
// the two-second latency aim: background chunks are transcribed without their
// neighbours' context, so they are never shorter than the 20-second floor.
// The per-second cost is the median of recent observations of at least 12 s
// of audio, never faster than the conservative seed. 1.4.5-1.4.7 kept the
// maximum ever seen instead, and short chunks (dominated by fixed overhead)
// ratcheted the threshold down to 1 s for the life of the process.
struct DictationSegmentBudget {
    static constexpr double minimum_pause_search_seconds{20.0};
    static constexpr double minimum_observed_audio_seconds{12.0};
    static constexpr double calibrated_seconds_per_audio_second{0.503 / 12.0};
    static constexpr std::size_t recent_observation_limit{5};

    double target_seconds{2.0};
    double insertion_seconds{0.35};
    double headroom_seconds{0.5};

    [[nodiscard]] double processing_seconds_per_audio_second() const noexcept;
    [[nodiscard]] double pause_search_seconds() const noexcept;
    void observe(double audio_seconds, double processing_seconds) noexcept;
    [[nodiscard]] static bool has_quiet_tail(
        const std::vector<float>& samples, unsigned sample_rate) noexcept;

private:
    std::array<double, recent_observation_limit> recent_rates_{};
    std::size_t recent_count_{0};
    std::size_t recent_next_{0};
};

// Synchronous, single-executor state machine. No microphone, UI or persistence
// access: append can never insert. Failures poison this session so the caller
// must explicitly retry the retained original audio, never skip missing words.
class IncrementalDictation {
public:
    using Transcribe = std::function<std::string(const Utterance&)>;
    using Transform = std::function<std::string(const std::string&)>;
    using Split = std::function<std::pair<std::string, std::string>(const std::string&)>;
    using Cancelled = std::function<bool()>;

    void append(const Utterance& audio, const Transcribe& transcribe,
                const Transform& transform, const Split& hold_last_sentence,
                const Cancelled& cancelled);
    [[nodiscard]] std::string finish(
        const Utterance& audio, const Transcribe& transcribe,
        const Transform& transform, const Cancelled& cancelled);

private:
    void advance(const Utterance& audio, bool final, const Transcribe& transcribe,
                 const Transform& transform, const Split& split,
                 const Cancelled& cancelled);
    std::string completed_;
    std::string pending_;
    bool closed_{false};
    bool failed_{false};
};

} // namespace localflow::core
