#pragma once

#ifdef _WIN32

#include <mutex>
#include <string>
#include <system_error>

namespace localflow::windows {

/// Temporarily scales the current default playback endpoint and restores the
/// exact endpoint/value later, even if the user changes defaults mid-hold.
/// Mute state is never altered.
class SystemAudioDucker final {
public:
    SystemAudioDucker() = default;
    ~SystemAudioDucker();

    SystemAudioDucker(const SystemAudioDucker&) = delete;
    SystemAudioDucker& operator=(const SystemAudioDucker&) = delete;

    [[nodiscard]] std::error_code duck(float scale = 0.2F);
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
