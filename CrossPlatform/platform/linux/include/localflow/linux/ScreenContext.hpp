#pragma once

#include "localflow/linux/Capabilities.hpp"
#include "localflow/linux/Status.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace localflow::platform::linux {

struct ApplicationInfo {
    std::string name;
    std::string applicationId;
    std::string windowTitle;
    std::int64_t processId{-1};
};

enum class PixelFormat {
    bgra8,
    rgba8,
};

struct ScreenFrame {
    int width{0};
    int height{0};
    int bytesPerRow{0};
    PixelFormat pixelFormat{PixelFormat::bgra8};
    std::vector<std::uint8_t> pixels;
};

class ScreenContextBackend {
public:
    virtual ~ScreenContextBackend() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual Result<ApplicationInfo> activeApplication() = 0;
    [[nodiscard]] virtual Result<ScreenFrame> captureContextFrame() = 0;
};

// Boundary for the ScreenCast portal + PipeWire frame consumer. The first
// start may display the desktop's chooser. Implementations should persist the
// portal restore token and reuse it when the compositor allows that.
class ScreenCastPortal {
public:
    virtual ~ScreenCastPortal() = default;

    virtual Status ensureSession() = 0;
    [[nodiscard]] virtual Result<ScreenFrame> latestFrame() = 0;
    virtual void close() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ScreenContextBackend> makeScreenContextBackend(
    SessionType session,
    std::shared_ptr<ScreenCastPortal> portal = {});

}  // namespace localflow::platform::linux
