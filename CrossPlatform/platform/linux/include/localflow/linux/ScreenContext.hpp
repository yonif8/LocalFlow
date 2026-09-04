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

// Boundary for org.freedesktop.portal.Screenshot. The first capture may show
// a desktop consent dialog. Unlike ScreenCast, this does not leave a screen-
// sharing session or indicator running between push-to-talk invocations.
class ScreenshotPortal {
public:
    virtual ~ScreenshotPortal() = default;

    [[nodiscard]] virtual Result<ScreenFrame> captureFrame() = 0;
    virtual void close() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ScreenContextBackend> makeScreenContextBackend(
    SessionType session,
    std::shared_ptr<ScreenshotPortal> portal = {});

}  // namespace localflow::platform::linux
