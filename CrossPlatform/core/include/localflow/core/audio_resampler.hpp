#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace localflow::core {

// Stateful, band-limited mono float-PCM resampler for Parakeet's 16 kHz input.
//
// process() may retain a short filter tail and return no samples for very short
// input chunks. finish() flushes that tail with edge extension, preserving the
// complete utterance duration without injecting silence at either boundary.
class MonoResampler16k {
public:
    static constexpr std::uint32_t output_sample_rate_hz = 16'000;
    static constexpr std::uint32_t minimum_input_sample_rate_hz = 8'000;
    static constexpr std::uint32_t maximum_input_sample_rate_hz = 96'000;

    explicit MonoResampler16k(std::uint32_t input_sample_rate_hz);
    ~MonoResampler16k();

    MonoResampler16k(MonoResampler16k&&) noexcept;
    MonoResampler16k& operator=(MonoResampler16k&&) noexcept;
    MonoResampler16k(const MonoResampler16k&) = delete;
    MonoResampler16k& operator=(const MonoResampler16k&) = delete;

    // Throws std::invalid_argument if samples is null for a non-empty chunk or
    // if any sample is NaN/infinite. Validation is atomic: a rejected chunk
    // does not alter stream state.
    [[nodiscard]] std::vector<float> process(const float* samples, std::size_t count);
    [[nodiscard]] std::vector<float> process(const std::vector<float>& samples) {
        return process(samples.data(), samples.size());
    }

    // Idempotent. The first call emits all delayed output; later calls return
    // an empty vector. process() cannot be called again until reset().
    [[nodiscard]] std::vector<float> finish();

    // Starts a new utterance at the same input rate and retains coefficient
    // tables already built for that rate.
    void reset() noexcept;

    [[nodiscard]] std::uint32_t input_sample_rate_hz() const noexcept;
    [[nodiscard]] std::uint64_t input_samples_received() const noexcept;
    [[nodiscard]] std::uint64_t output_samples_emitted() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience batch API. The result contains ceil(input_count * 16000 / rate)
// samples so the final fraction of the utterance is never silently discarded.
[[nodiscard]] std::vector<float> resample_mono_to_16khz(
    const std::vector<float>& samples,
    std::uint32_t input_sample_rate_hz);

}  // namespace localflow::core

