#pragma once

#ifdef _WIN32

#include <mutex>
#include <string>
#include <system_error>

namespace localflow::windows {

/// Temporarily lowers the current default playback endpoint to an absolute
/// volume scalar (0 = silent) and restores the exact endpoint/value later,
/// even if the user changes defaults mid-hold. An endpoint already at or
/// below the level is left alone. Mute state is never altered.
class SystemAudioDucker final {
public:
    SystemAudioDucker() = default;
    ~SystemAudioDucker();

    SystemAudioDucker(const SystemAudioDucker&) = delete;
    SystemAudioDucker& operator=(const SystemAudioDucker&) = delete;

    [[nodiscard]] std::error_code duck(float level = 0.2F);
    [[nodiscard]] std::error_code restore() noexcept;
    [[nodiscard]] bool is_ducked() const noexcept;

private:
    mutable std::mutex mutex_;
    std::wstring endpoint_id_;
    float original_volume_{1.0F};
    float applied_ducked_volume_{1.0F};
    bool active_{false};
};

}  // namespace localflow::windows

#endif
