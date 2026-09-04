#pragma once

#include "localflow/linux/Capabilities.hpp"
#include "localflow/linux/Status.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace localflow::platform::linux {

enum class FieldSecurity {
    unknown,
    non_secure,
    secure,
};

struct FocusedAccessibleTarget {
    // The AT-SPI application's unique bus name and the accessible's object
    // path form the runtime identity. accessibleId is toolkit-provided and may
    // be empty, but is retained as an additional stable identity signal.
    std::string busName;
    std::string objectPath;
    std::string accessibleId;
    std::string role;
    bool focused{false};
    bool editable{false};
    FieldSecurity security{FieldSecurity::unknown};
};

struct ApplicationInfo {
    std::string name;
    std::string applicationId;
    std::string windowTitle;
    std::int64_t processId{-1};
    // Populated by X11 screen-context snapshots. It is intentionally opaque to
    // shared code and lets a delayed OCR worker prove that the same native
    // window is still active before capturing pixels.
    std::uint64_t nativeWindowId{0};
    std::optional<FocusedAccessibleTarget> focusedTarget;
};

// Captures the exact focused AT-SPI object. Implementations bound tree walks,
// bus calls, and depth; an unidentified target is an error rather than a
// partially populated success.
class FocusedTargetProvider {
public:
    virtual ~FocusedTargetProvider() = default;
    [[nodiscard]] virtual Result<ApplicationInfo> snapshotFocusedTarget() = 0;
};

// Pure validation helper shared by insertion backends and deterministic tests.
// Success means both snapshots identify the exact same focused, editable,
// non-secure accessible object.
[[nodiscard]] Status validateFocusedTarget(
    const ApplicationInfo& expected,
    const ApplicationInfo& current);

// Validates either an exact native X11 window identity or, where no native
// identity exists, the exact non-secure AT-SPI field identity.
[[nodiscard]] Status validateScreenCaptureTarget(
    const ApplicationInfo& expected,
    const ApplicationInfo& current);

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

    // Production callers should use this overload so an asynchronous capture
    // cannot silently switch to a different app/window after push-to-talk.
    [[nodiscard]] virtual Result<ScreenFrame> captureContextFrame(
        const ApplicationInfo& expectedTarget);

    // Interrupts an in-flight portal request during listener shutdown. Native
    // synchronous backends may keep the default no-op implementation.
    virtual void cancelPendingCapture() noexcept {}
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
    std::shared_ptr<ScreenshotPortal> portal = {},
    std::shared_ptr<FocusedTargetProvider> focusedTargets = {});

[[nodiscard]] std::shared_ptr<FocusedTargetProvider>
makeAtSpiFocusedTargetProvider(const CapabilityReport& report);

}  // namespace localflow::platform::linux
