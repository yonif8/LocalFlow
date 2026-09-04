#include "localflow/core/audio_resampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace localflow::core {
namespace {

constexpr std::int64_t filter_radius = 64;
constexpr std::int64_t first_tap_offset = 1 - filter_radius;
constexpr std::int64_t last_tap_offset = filter_radius;
constexpr std::size_t tap_count = static_cast<std::size_t>(
    last_tap_offset - first_tap_offset + 1);
constexpr double pi = 3.141592653589793238462643383279502884;

std::uint64_t complete_output_count(
    std::uint64_t input_count,
    std::uint32_t input_rate) {
    const std::uint64_t whole_seconds = input_count / input_rate;
    const std::uint64_t remainder = input_count % input_rate;
    if (whole_seconds > std::numeric_limits<std::uint64_t>::max()
            / MonoResampler16k::output_sample_rate_hz) {
        throw std::length_error("resampled audio is too large");
    }
    return whole_seconds * MonoResampler16k::output_sample_rate_hz
        + (remainder * MonoResampler16k::output_sample_rate_hz + input_rate - 1)
            / input_rate;
}

}  // namespace

struct MonoResampler16k::Impl {
    explicit Impl(std::uint32_t rate) : input_rate(rate) {
        if (rate < minimum_input_sample_rate_hz
            || rate > maximum_input_sample_rate_hz) {
            throw std::invalid_argument("input sample rate must be between 8000 and 96000 Hz");
        }
    }

    std::uint32_t input_rate;
    std::deque<float> input_buffer;
    std::uint64_t buffer_start_index{0};
    std::uint64_t total_input_samples{0};
    std::uint64_t next_output_index{0};
    std::uint64_t source_floor{0};
    std::uint32_t phase_numerator{0};
    float first_sample{0.0F};
    float last_sample{0.0F};
    bool has_samples{false};
    bool finished{false};
    std::unordered_map<std::uint32_t, std::vector<double>> coefficient_cache;

    const std::vector<double>& coefficients(std::uint32_t phase) {
        const auto cached = coefficient_cache.find(phase);
        if (cached != coefficient_cache.end()) {
            return cached->second;
        }

        const double fraction = static_cast<double>(phase)
            / static_cast<double>(output_sample_rate_hz);
        // Leave a narrow transition band below the lower Nyquist frequency.
        // Speech loses no meaningful content, while downsampling does not fold
        // energy immediately above 8 kHz into Parakeet's input band.
        const double cutoff = 0.96 * std::min(
            1.0,
            static_cast<double>(output_sample_rate_hz)
                / static_cast<double>(input_rate));
        std::vector<double> values;
        values.reserve(tap_count);
        double coefficient_sum = 0.0;
        for (std::int64_t offset = first_tap_offset;
             offset <= last_tap_offset; ++offset) {
            const double distance = static_cast<double>(offset) - fraction;
            const double absolute_distance = std::abs(distance);
            double coefficient = 0.0;
            if (absolute_distance < static_cast<double>(filter_radius)) {
                const double sinc = absolute_distance < 1.0e-12
                    ? cutoff
                    : std::sin(pi * cutoff * distance) / (pi * distance);
                const double position = distance / static_cast<double>(filter_radius);
                const double window = 0.42
                    + 0.5 * std::cos(pi * position)
                    + 0.08 * std::cos(2.0 * pi * position);
                coefficient = sinc * window;
            }
            values.push_back(coefficient);
            coefficient_sum += coefficient;
        }
        if (std::abs(coefficient_sum) < 1.0e-15) {
            throw std::runtime_error("resampler filter normalization failed");
        }
        for (double& value : values) {
            value /= coefficient_sum;
        }
        return coefficient_cache.emplace(phase, std::move(values)).first->second;
    }

    float sample_at(std::uint64_t absolute_index, bool final) const {
        if (absolute_index >= total_input_samples) {
            if (!final) {
                throw std::logic_error("resampler requested unavailable future input");
            }
            return last_sample;
        }
        if (absolute_index < buffer_start_index) {
            throw std::logic_error("resampler discarded live input history");
        }
        return input_buffer[static_cast<std::size_t>(absolute_index - buffer_start_index)];
    }

    float sample_at_offset(std::int64_t offset, bool final) const {
        if (offset < 0) {
            const auto magnitude = static_cast<std::uint64_t>(-offset);
            if (source_floor < magnitude) {
                return first_sample;
            }
            return sample_at(source_floor - magnitude, final);
        }
        const auto magnitude = static_cast<std::uint64_t>(offset);
        if (source_floor > std::numeric_limits<std::uint64_t>::max() - magnitude) {
            throw std::length_error("resampler source position overflow");
        }
        return sample_at(source_floor + magnitude, final);
    }

    void advance_output_position() noexcept {
        ++next_output_index;
        const std::uint64_t advanced_phase = static_cast<std::uint64_t>(phase_numerator)
            + input_rate;
        source_floor += advanced_phase / output_sample_rate_hz;
        phase_numerator = static_cast<std::uint32_t>(
            advanced_phase % output_sample_rate_hz);
    }

    void discard_consumed_history() noexcept {
        const auto earliest_needed = source_floor
                >= static_cast<std::uint64_t>(filter_radius - 1)
            ? source_floor - static_cast<std::uint64_t>(filter_radius - 1)
            : 0;
        while (!input_buffer.empty() && buffer_start_index < earliest_needed) {
            input_buffer.pop_front();
            ++buffer_start_index;
        }
    }

    std::vector<float> produce(bool final) {
        std::vector<float> output;
        if (!has_samples) {
            return output;
        }
        const std::uint64_t final_count = final
            ? complete_output_count(total_input_samples, input_rate)
            : std::numeric_limits<std::uint64_t>::max();
        if (final && final_count > next_output_index) {
            const auto remaining = final_count - next_output_index;
            if (remaining <= std::numeric_limits<std::size_t>::max()) {
                output.reserve(static_cast<std::size_t>(remaining));
            }
        }

        while (next_output_index < final_count) {
            if (!final
                && (source_floor > std::numeric_limits<std::uint64_t>::max()
                        - static_cast<std::uint64_t>(filter_radius)
                    || source_floor + static_cast<std::uint64_t>(filter_radius)
                        >= total_input_samples)) {
                break;
            }
            const auto& kernel = coefficients(phase_numerator);
            double value = 0.0;
            std::size_t kernel_index = 0;
            for (std::int64_t offset = first_tap_offset;
                 offset <= last_tap_offset; ++offset, ++kernel_index) {
                value += static_cast<double>(sample_at_offset(offset, final))
                    * kernel[kernel_index];
            }
            const double float_limit = static_cast<double>(std::numeric_limits<float>::max());
            value = std::max(-float_limit, std::min(float_limit, value));
            output.push_back(static_cast<float>(value));
            advance_output_position();
        }
        discard_consumed_history();
        return output;
    }

    void clear_stream() noexcept {
        input_buffer.clear();
        buffer_start_index = 0;
        total_input_samples = 0;
        next_output_index = 0;
        source_floor = 0;
        phase_numerator = 0;
        first_sample = 0.0F;
        last_sample = 0.0F;
        has_samples = false;
        finished = false;
    }
};

MonoResampler16k::MonoResampler16k(std::uint32_t input_sample_rate_hz)
    : impl_(std::make_unique<Impl>(input_sample_rate_hz)) {}

MonoResampler16k::~MonoResampler16k() = default;
MonoResampler16k::MonoResampler16k(MonoResampler16k&&) noexcept = default;
MonoResampler16k& MonoResampler16k::operator=(MonoResampler16k&&) noexcept = default;

std::vector<float> MonoResampler16k::process(const float* samples, std::size_t count) {
    if (impl_->finished) {
        throw std::logic_error("cannot process audio after finish; call reset first");
    }
    if (count != 0 && samples == nullptr) {
        throw std::invalid_argument("non-empty audio chunk has a null sample pointer");
    }
    // Validate the complete chunk before mutating state. A bad device buffer
    // can be dropped/reported without corrupting the surrounding utterance.
    for (std::size_t index = 0; index < count; ++index) {
        if (!std::isfinite(samples[index])) {
            throw std::invalid_argument("audio chunk contains a non-finite sample");
        }
    }
    if (count == 0) {
        return {};
    }
    if (impl_->total_input_samples
        > std::numeric_limits<std::uint64_t>::max() - count) {
        throw std::length_error("input audio is too large");
    }

    if (impl_->input_rate == output_sample_rate_hz) {
        if (!impl_->has_samples) {
            impl_->first_sample = samples[0];
            impl_->has_samples = true;
        }
        impl_->last_sample = samples[count - 1];
        impl_->total_input_samples += count;
        impl_->next_output_index += count;
        return {samples, samples + count};
    }

    if (!impl_->has_samples) {
        impl_->first_sample = samples[0];
        impl_->has_samples = true;
    }
    impl_->last_sample = samples[count - 1];
    impl_->input_buffer.insert(impl_->input_buffer.end(), samples, samples + count);
    impl_->total_input_samples += count;
    return impl_->produce(false);
}

std::vector<float> MonoResampler16k::finish() {
    if (impl_->finished) {
        return {};
    }
    std::vector<float> output;
    if (impl_->input_rate != output_sample_rate_hz) {
        output = impl_->produce(true);
    }
    impl_->finished = true;
    impl_->input_buffer.clear();
    return output;
}

void MonoResampler16k::reset() noexcept {
    impl_->clear_stream();
}

std::uint32_t MonoResampler16k::input_sample_rate_hz() const noexcept {
    return impl_->input_rate;
}

std::uint64_t MonoResampler16k::input_samples_received() const noexcept {
    return impl_->total_input_samples;
}

std::uint64_t MonoResampler16k::output_samples_emitted() const noexcept {
    return impl_->next_output_index;
}

std::vector<float> resample_mono_to_16khz(
    const std::vector<float>& samples,
    std::uint32_t input_sample_rate_hz) {
    MonoResampler16k resampler(input_sample_rate_hz);
    auto output = resampler.process(samples);
    auto tail = resampler.finish();
    output.insert(output.end(), tail.begin(), tail.end());
    return output;
}

}  // namespace localflow::core

