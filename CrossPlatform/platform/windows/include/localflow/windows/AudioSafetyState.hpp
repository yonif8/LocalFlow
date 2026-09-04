#pragma once

#include <chrono>
#include <cstdint>

namespace localflow::windows {

inline constexpr std::chrono::milliseconds kMaximumAudioTailDrain{250};

[[nodiscard]] std::chrono::milliseconds clamp_audio_tail_drain(
    std::chrono::milliseconds requested) noexcept;

/// Returns a ceil-rounded, WaitForSingleObject-compatible timeout. Zero means
/// that the bounded drain window has expired.
[[nodiscard]] std::uint32_t audio_tail_wait_milliseconds(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline) noexcept;

enum class VolumeRestoreDecision : std::uint8_t {
    restore_original,
    skip_not_active,
    skip_user_adjusted_volume,
};

/// A conditional restore is deliberately a compare-before-set operation. If
/// the current endpoint level no longer matches the level LocalFlow applied,
/// the user's newer choice wins. Mute is outside this policy and is never set.
[[nodiscard]] VolumeRestoreDecision decide_volume_restore(
    bool active,
    float current_volume,
    float applied_ducked_volume) noexcept;

}  // namespace localflow::windows
