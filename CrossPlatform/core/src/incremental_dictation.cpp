#include "localflow/core/incremental_dictation.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace localflow::core {
namespace {
std::string join(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + " " + right;
}
}

double DictationSegmentBudget::processing_seconds_per_audio_second() const noexcept {
    if (recent_count_ == 0) return calibrated_seconds_per_audio_second;
    std::array<double, recent_observation_limit> sorted = recent_rates_;
    std::sort(sorted.begin(), sorted.begin() + std::ptrdiff_t(recent_count_));
    const std::size_t mid = recent_count_ / 2;
    const double median = recent_count_ % 2 == 0 ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid];
    return std::max(calibrated_seconds_per_audio_second, median);
}

double DictationSegmentBudget::pause_search_seconds() const noexcept {
    return std::max(minimum_pause_search_seconds,
        (target_seconds - insertion_seconds - headroom_seconds)
            / std::max(0.001, processing_seconds_per_audio_second()));
}

void DictationSegmentBudget::observe(double audio, double processing) noexcept {
    if (!std::isfinite(audio) || audio < minimum_observed_audio_seconds ||
        !std::isfinite(processing) || processing <= 0) return;
    recent_rates_[recent_next_] = processing / audio;
    recent_next_ = (recent_next_ + 1) % recent_observation_limit;
    recent_count_ = std::min(recent_count_ + 1, recent_observation_limit);
}

bool DictationSegmentBudget::has_quiet_tail(const std::vector<float>& samples, unsigned rate) noexcept {
    const auto count = std::size_t(double(rate) * 0.3);
    if (count == 0 || samples.size() < count) return false;
    double sum = 0;
    for (std::size_t i = samples.size() - count; i < samples.size(); ++i) {
        if (!std::isfinite(samples[i])) return false;
        sum += double(samples[i]) * samples[i];
    }
    return std::sqrt(sum / double(count)) < 0.009;
}

void IncrementalDictation::append(
    const Utterance& audio, const Transcribe& transcribe, const Transform& transform,
    const Split& split, const Cancelled& cancelled) {
    advance(audio, false, transcribe, transform, split, cancelled);
}

std::string IncrementalDictation::finish(
    const Utterance& audio, const Transcribe& transcribe,
    const Transform& transform, const Cancelled& cancelled) {
    advance(audio, true, transcribe, transform, {}, cancelled);
    return completed_;
}

void IncrementalDictation::advance(
    const Utterance& audio, bool final, const Transcribe& transcribe,
    const Transform& transform, const Split& split, const Cancelled& cancelled) {
    if (closed_ || failed_) throw std::runtime_error("Dictation session is no longer active");
    try {
        const auto check = [&] {
            if (cancelled && cancelled()) throw std::runtime_error("Dictation cancelled");
        };
        check();
        std::string raw;
        if (audio.samples.size() > 256 && std::any_of(audio.samples.begin(), audio.samples.end(),
                [](float value) { return std::isfinite(value) && std::abs(value) > 0.0001f; })) {
            raw = transcribe(audio);
        }
        check();
        const auto combined = join(pending_, detail::trim_unicode_whitespace(raw));
        const auto parts = final ? std::make_pair(combined, std::string{}) : split(combined);
        if (!parts.first.empty()) {
            const auto polished = transform(parts.first);
            check();
            completed_ = join(completed_, polished);
        }
        pending_ = parts.second;
        closed_ = final;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

} // namespace localflow::core
