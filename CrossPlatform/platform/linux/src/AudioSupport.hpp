#pragma once

#include "localflow/linux/Audio.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace localflow::platform::linux::detail {

struct OutputVolumeState {
    float level{1.0F};
    bool muted{false};
};

// Parsing is kept outside the process adapters so malformed desktop-service
// output can be tested without a running audio server.
[[nodiscard]] std::vector<AudioDevice> parsePactlSources(
    const std::string& sources,
    const std::string& defaultSource);
[[nodiscard]] Result<OutputVolumeState> parseWpctlVolume(
    const std::string& output);
[[nodiscard]] Result<std::string> parseWpctlObjectId(
    const std::string& output);
[[nodiscard]] Result<std::string> parseAudioServiceIdentifier(
    const std::string& output);
[[nodiscard]] Result<float> parsePactlVolume(const std::string& output);
[[nodiscard]] Result<bool> parsePactlMute(const std::string& output);

[[nodiscard]] bool shouldRestoreDuckedVolume(
    const OutputVolumeState& applied,
    const OutputVolumeState& current) noexcept;

class S16LePcmDecoder {
public:
    void decode(
        const std::uint8_t* bytes,
        std::size_t count,
        std::vector<float>& output);
    void reset() noexcept;

private:
    bool hasPendingByte_{false};
    std::uint8_t pendingByte_{0};
};

}  // namespace localflow::platform::linux::detail
