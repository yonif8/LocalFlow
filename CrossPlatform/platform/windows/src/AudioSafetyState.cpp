#include "localflow/windows/AudioSafetyState.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace localflow::windows {

std::chrono::milliseconds clamp_audio_tail_drain(
    const std::chrono::milliseconds requested) noexcept {
    return std::clamp(requested, std::chrono::milliseconds::zero(),
                      kMaximumAudioTailDrain);
}

std::uint32_t audio_tail_wait_milliseconds(
    const std::chrono::steady_clock::time_point now,
    const std::chrono::steady_clock::time_point deadline) noexcept {
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = deadline - now;
    const auto whole_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    const bool has_fraction = whole_milliseconds < remaining;
    const auto rounded = whole_milliseconds +
        (has_fraction ? std::chrono::milliseconds{1} : std::chrono::milliseconds::zero());
    const auto bounded = std::min(rounded, kMaximumAudioTailDrain);
    return static_cast<std::uint32_t>(bounded.count());
}

VolumeRestoreDecision decide_volume_restore(
    const bool active,
    const float current_volume,
    const float applied_ducked_volume) noexcept {
    if (!active) {
        return VolumeRestoreDecision::skip_not_active;
    }
    if (!std::isfinite(current_volume) || !std::isfinite(applied_ducked_volume)) {
        return VolumeRestoreDecision::skip_user_adjusted_volume;
    }
    constexpr float kEndpointRoundTripTolerance = 1.0e-6F;
    return std::fabs(current_volume - applied_ducked_volume)
            <= kEndpointRoundTripTolerance
        ? VolumeRestoreDecision::restore_original
        : VolumeRestoreDecision::skip_user_adjusted_volume;
}

}  // namespace localflow::windows
