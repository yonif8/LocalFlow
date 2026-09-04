#include "localflow/linux/ScreenContext.hpp"

#include "InternalFactories.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace localflow::platform::linux {
namespace {

class PortalScreenContext final : public ScreenContextBackend {
public:
    explicit PortalScreenContext(std::shared_ptr<ScreenshotPortal> portal)
        : portal_(std::move(portal)) {}

    ~PortalScreenContext() override {
        if (portal_) {
            portal_->close();
        }
    }

    std::string name() const override {
        return "xdg-desktop-portal Screenshot";
    }

    Result<ApplicationInfo> activeApplication() override {
        return Result<ApplicationInfo>::failure(Status::failure(
            ErrorCode::unsupported_session,
            "Wayland does not expose a universal active-application API.",
            "Use AT-SPI metadata where available and associate it with the user-selected ScreenCast stream."));
    }

    Result<ScreenFrame> captureContextFrame() override {
        if (!portal_) {
            return Result<ScreenFrame>::failure(Status::failure(
                ErrorCode::not_configured,
                "The Screenshot portal transport was not configured.",
                "Install Qt DBus support and xdg-desktop-portal before enabling Screen Terminology."));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return portal_->captureFrame();
    }

private:
    std::shared_ptr<ScreenshotPortal> portal_;
    std::mutex mutex_;
};

class UnavailableScreenContext final : public ScreenContextBackend {
public:
    explicit UnavailableScreenContext(Status status) : status_(std::move(status)) {}

    std::string name() const override { return "unavailable"; }

    Result<ApplicationInfo> activeApplication() override {
        return Result<ApplicationInfo>::failure(status_);
    }

    Result<ScreenFrame> captureContextFrame() override {
        return Result<ScreenFrame>::failure(status_);
    }

private:
    Status status_;
};

}  // namespace

std::unique_ptr<ScreenContextBackend> makeScreenContextBackend(
    SessionType session,
    std::shared_ptr<ScreenshotPortal> portal) {
    switch (session) {
        case SessionType::x11:
            return detail::makeX11ScreenContextBackend();
        case SessionType::wayland:
            if (!portal) portal = detail::makeQDbusScreenshotPortal();
            return std::make_unique<PortalScreenContext>(std::move(portal));
        case SessionType::unknown:
            return std::make_unique<UnavailableScreenContext>(Status::failure(
                ErrorCode::unsupported_session,
                "Screen context requires an X11 or Wayland graphical session."));
    }
    return std::make_unique<UnavailableScreenContext>(Status::failure(
        ErrorCode::internal_error,
        "Unknown Linux session type."));
}

}  // namespace localflow::platform::linux
