#include "localflow/linux/ScreenContext.hpp"

#include "InternalFactories.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace localflow::platform::linux {
namespace {

class PortalScreenContext final : public ScreenContextBackend {
public:
    explicit PortalScreenContext(std::shared_ptr<ScreenCastPortal> portal)
        : portal_(std::move(portal)) {}

    ~PortalScreenContext() override {
        if (portal_) {
            portal_->close();
        }
    }

    std::string name() const override {
        return "xdg-desktop-portal ScreenCast + PipeWire";
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
                "The ScreenCast portal transport was not configured.",
                "Attach the Qt/QDBus or GDBus ScreenCast + PipeWire transport before enabling Screen Terminology."));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessionReady_) {
            const auto status = portal_->ensureSession();
            if (!status.ok()) {
                return Result<ScreenFrame>::failure(status);
            }
            sessionReady_ = true;
        }
        return portal_->latestFrame();
    }

private:
    std::shared_ptr<ScreenCastPortal> portal_;
    std::mutex mutex_;
    bool sessionReady_{false};
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
    std::shared_ptr<ScreenCastPortal> portal) {
    switch (session) {
        case SessionType::x11:
            return detail::makeX11ScreenContextBackend();
        case SessionType::wayland:
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
