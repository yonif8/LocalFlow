#pragma once

#include "localflow/core/contracts.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace localflow::core {

// Same two-second policy as macOS. The initial calibration is a conservative
// seed, not a timing claim for Windows/Linux hardware; live measurements only
// tighten it. The platform owns execution and the audio snapshot lifetime.
struct DictationSegmentBudget {
    double target_seconds{2.0};
    double insertion_seconds{0.35};
    double headroom_seconds{0.5};
    double processing_seconds_per_audio_second{0.503 / 12.0};

    [[nodiscard]] double pause_search_seconds() const noexcept;
    void observe(double audio_seconds, double processing_seconds) noexcept;
    [[nodiscard]] static bool has_quiet_tail(
        const std::vector<float>& samples, unsigned sample_rate) noexcept;
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
