#include "AudioSupport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace localflow::platform::linux::detail {
namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

bool parseFloatBeforePercent(const std::string& output, float& value) {
    const auto percent = output.find('%');
    if (percent == std::string::npos) return false;
    auto begin = percent;
    while (begin > 0) {
        const char character = output[begin - 1];
        if (std::isdigit(static_cast<unsigned char>(character)) == 0 && character != '.') break;
        --begin;
    }
    if (begin == percent) return false;
    try {
        value = std::stof(output.substr(begin, percent - begin));
        return std::isfinite(value);
    } catch (...) {
        return false;
    }
}

}  // namespace

std::vector<AudioDevice> parsePactlSources(
    const std::string& sources,
    const std::string& defaultSource) {
    struct ParsedSource {
        std::string id;
        std::string description;
        bool monitor{false};
    };

    std::vector<ParsedSource> parsed;
    ParsedSource current;
    bool insideSource = false;
    auto finish = [&] {
        if (insideSource && !current.id.empty()) {
            current.monitor = current.monitor || endsWith(current.id, ".monitor");
            parsed.push_back(std::move(current));
        }
        current = {};
    };

    std::size_t offset = 0;
    while (offset <= sources.size()) {
        const auto end = sources.find('\n', offset);
        const auto line = trim(sources.substr(
            offset,
            end == std::string::npos ? std::string::npos : end - offset));
        if (startsWith(line, "Source #")) {
            finish();
            insideSource = true;
        } else if (insideSource && startsWith(line, "Name:")) {
            current.id = trim(line.substr(5));
        } else if (insideSource && startsWith(line, "Description:")) {
            current.description = trim(line.substr(12));
        } else if (insideSource && startsWith(line, "Monitor of Sink:")) {
            const auto sink = trim(line.substr(16));
            current.monitor = !sink.empty() && sink != "n/a";
        }
        if (end == std::string::npos) break;
        offset = end + 1;
    }
    finish();

    const auto wantedDefault = trim(defaultSource);
    std::vector<AudioDevice> result;
    for (const auto& source : parsed) {
        if (source.monitor) continue;
        const auto duplicate = std::find_if(
            result.begin(), result.end(), [&](const AudioDevice& device) {
                return device.id == source.id;
            });
        if (duplicate != result.end()) continue;
        result.push_back({
            source.id,
            source.description.empty() ? source.id : source.description,
            source.id == wantedDefault,
        });
    }
    return result;
}

Result<OutputVolumeState> parseWpctlVolume(const std::string& output) {
    const auto marker = output.find("Volume:");
    if (marker == std::string::npos) {
        return Result<OutputVolumeState>::failure(Status::failure(
            ErrorCode::protocol_error, "wpctl returned an unrecognized volume response."));
    }
    try {
        const float level = std::stof(output.substr(marker + 7));
        if (!std::isfinite(level) || level < 0.0F) throw std::invalid_argument("volume");
        return Result<OutputVolumeState>::success({
            level,
            output.find("[MUTED]", marker) != std::string::npos,
        });
    } catch (...) {
        return Result<OutputVolumeState>::failure(Status::failure(
            ErrorCode::protocol_error, "Could not parse the wpctl output volume."));
    }
}

Result<std::string> parseWpctlObjectId(const std::string& output) {
    const auto marker = output.find("id ");
    if (marker != std::string::npos) {
        const auto first = output.find_first_of("0123456789", marker + 3);
        if (first != std::string::npos) {
            const auto end = output.find_first_not_of("0123456789", first);
            const auto id = output.substr(first, end - first);
            if (!id.empty()) return Result<std::string>::success(id);
        }
    }
    return Result<std::string>::failure(Status::failure(
        ErrorCode::protocol_error,
        "Could not identify the default PipeWire output device."));
}

Result<std::string> parseAudioServiceIdentifier(const std::string& output) {
    const auto identifier = trim(output);
    if (identifier.empty() ||
        std::any_of(identifier.begin(), identifier.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        return Result<std::string>::failure(Status::failure(
            ErrorCode::protocol_error,
            "The audio service returned an invalid device identifier."));
    }
    return Result<std::string>::success(identifier);
}

Result<float> parsePactlVolume(const std::string& output) {
    float percent = 0.0F;
    if (!parseFloatBeforePercent(output, percent) || percent < 0.0F) {
        return Result<float>::failure(Status::failure(
            ErrorCode::protocol_error, "Could not parse the pactl output volume."));
    }
    return Result<float>::success(percent / 100.0F);
}

Result<bool> parsePactlMute(const std::string& output) {
    auto normalized = trim(output);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const auto marker = normalized.find("mute:");
    const auto value = marker == std::string::npos
        ? normalized
        : trim(normalized.substr(marker + 5));
    if (startsWith(value, "yes")) return Result<bool>::success(true);
    if (startsWith(value, "no")) return Result<bool>::success(false);
    return Result<bool>::failure(Status::failure(
        ErrorCode::protocol_error, "Could not parse the pactl mute state."));
}

bool shouldRestoreDuckedVolume(
    const OutputVolumeState& applied,
    const OutputVolumeState& current) noexcept {
    constexpr float tolerance = 0.0001F;
    return applied.muted == current.muted &&
        std::fabs(applied.level - current.level) <= tolerance;
}

void S16LePcmDecoder::decode(
    const std::uint8_t* bytes,
    std::size_t count,
    std::vector<float>& output) {
    output.clear();
    output.reserve((count + (hasPendingByte_ ? 1U : 0U)) / 2U);
    std::size_t offset = 0;
    if (hasPendingByte_ && count > 0) {
        const auto bits = static_cast<std::uint16_t>(pendingByte_) |
            (static_cast<std::uint16_t>(bytes[0]) << 8U);
        output.push_back(static_cast<float>(static_cast<std::int16_t>(bits)) / 32768.0F);
        hasPendingByte_ = false;
        offset = 1;
    }
    while (offset + 1 < count) {
        const auto bits = static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
        output.push_back(static_cast<float>(static_cast<std::int16_t>(bits)) / 32768.0F);
        offset += 2;
    }
    if (offset < count) {
        pendingByte_ = bytes[offset];
        hasPendingByte_ = true;
    }
}

void S16LePcmDecoder::reset() noexcept {
    hasPendingByte_ = false;
    pendingByte_ = 0;
}

}  // namespace localflow::platform::linux::detail
