#include "localflow/linux/ScreenContext.hpp"

#include "InternalFactories.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace localflow::platform::linux {
namespace {

Status invalidSnapshotStatus(const char* moment) {
    return Status::failure(
        ErrorCode::not_configured,
        std::string("LocalFlow could not verify the focused field ") + moment + ".",
        "Keep the destination field focused and try dictating again.");
}

Status usableTargetStatus(const ApplicationInfo& snapshot, const char* moment) {
    if (snapshot.processId <= 0 || snapshot.applicationId.empty() ||
        !snapshot.focusedTarget.has_value() ||
        snapshot.focusedTarget->busName.empty() ||
        snapshot.focusedTarget->objectPath.empty()) {
        return invalidSnapshotStatus(moment);
    }
    const auto& target = *snapshot.focusedTarget;
    if (!target.focused) {
        return Status::failure(
            ErrorCode::focus_changed,
            std::string("The destination field is no longer focused ") + moment + ".",
            "Return focus to the intended field and paste the transcript manually.");
    }
    if (target.security != FieldSecurity::non_secure) {
        return Status::failure(
            ErrorCode::secure_field,
            target.security == FieldSecurity::secure
                ? "LocalFlow will not insert or paste into a password or protected field."
                : "LocalFlow could not prove that the focused field is non-secure.",
            "Choose a normal text field and try again.");
    }
    if (!target.editable) {
        return Status::failure(
            ErrorCode::not_editable,
            std::string("The focused target is not an editable text field ") + moment + ".",
            "Choose an editable, non-password text field and try again.");
    }
    return Status::success();
}

class PortalScreenContext final : public ScreenContextBackend {
public:
    PortalScreenContext(
        std::shared_ptr<ScreenshotPortal> portal,
        std::shared_ptr<FocusedTargetProvider> focusedTargets)
        : portal_(std::move(portal)),
          focusedTargets_(std::move(focusedTargets)) {}

    ~PortalScreenContext() override {
        if (portal_) {
            portal_->close();
        }
    }

    std::string name() const override {
        return "xdg-desktop-portal Screenshot";
    }

    Result<ApplicationInfo> activeApplication() override {
        if (!focusedTargets_) {
            return Result<ApplicationInfo>::failure(Status::failure(
                ErrorCode::not_configured,
                "No AT-SPI focused-target provider was configured for Wayland.",
                "Enable desktop accessibility and restart LocalFlow."));
        }
        return focusedTargets_->snapshotFocusedTarget();
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

    void cancelPendingCapture() noexcept override {
        if (portal_) portal_->close();
    }

private:
    std::shared_ptr<ScreenshotPortal> portal_;
    std::shared_ptr<FocusedTargetProvider> focusedTargets_;
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

class UnavailableFocusedTargetProvider final : public FocusedTargetProvider {
public:
    explicit UnavailableFocusedTargetProvider(Status status)
        : status_(std::move(status)) {}

    Result<ApplicationInfo> snapshotFocusedTarget() override {
        return Result<ApplicationInfo>::failure(status_);
    }

private:
    Status status_;
};

}  // namespace

Status validateFocusedTarget(
    const ApplicationInfo& expected,
    const ApplicationInfo& current) {
    auto status = usableTargetStatus(expected, "when dictation began");
    if (!status.ok()) return status;
    status = usableTargetStatus(current, "before insertion");
    if (!status.ok()) return status;

    const auto& expectedTarget = *expected.focusedTarget;
    const auto& currentTarget = *current.focusedTarget;
    const bool sameApplication = expected.processId == current.processId &&
                                 expected.applicationId == current.applicationId;
    const bool sameAccessible =
        expectedTarget.busName == currentTarget.busName &&
        expectedTarget.objectPath == currentTarget.objectPath &&
        expectedTarget.accessibleId == currentTarget.accessibleId;
    if (!sameApplication || !sameAccessible) {
        return Status::failure(
            ErrorCode::focus_changed,
            "The focused field changed while LocalFlow was processing the recording.",
            "Nothing was pasted. Return to the intended field and paste the transcript manually.");
    }
    return Status::success();
}

Status validateScreenCaptureTarget(
    const ApplicationInfo& expected,
    const ApplicationInfo& current) {
    if (expected.nativeWindowId != 0 || current.nativeWindowId != 0) {
        const bool exactWindow = expected.nativeWindowId != 0 &&
                                 current.nativeWindowId == expected.nativeWindowId;
        const bool exactProcess = expected.processId > 0 &&
                                  current.processId == expected.processId;
        if (!exactWindow || !exactProcess) {
            return Status::failure(
                ErrorCode::focus_changed,
                "The active window changed before LocalFlow could capture terminology context.",
                "Screen terminology was skipped for this dictation.");
        }
        return Status::success();
    }
    return validateFocusedTarget(expected, current);
}

Result<ScreenFrame> ScreenContextBackend::captureContextFrame(
    const ApplicationInfo& expectedTarget) {
    const auto before = activeApplication();
    if (!before) return Result<ScreenFrame>::failure(before.status());
    auto validation = validateScreenCaptureTarget(expectedTarget, before.value());
    if (!validation.ok()) return Result<ScreenFrame>::failure(std::move(validation));

    auto captured = captureContextFrame();
    if (!captured) return captured;

    const auto after = activeApplication();
    if (!after) return Result<ScreenFrame>::failure(after.status());
    validation = validateScreenCaptureTarget(expectedTarget, after.value());
    if (!validation.ok()) return Result<ScreenFrame>::failure(std::move(validation));
    return captured;
}

std::unique_ptr<ScreenContextBackend> makeScreenContextBackend(
    SessionType session,
    std::shared_ptr<ScreenshotPortal> portal,
    std::shared_ptr<FocusedTargetProvider> focusedTargets) {
    switch (session) {
        case SessionType::x11:
            return detail::makeX11ScreenContextBackend();
        case SessionType::wayland:
            if (!portal) portal = detail::makeQDbusScreenshotPortal();
            if (!focusedTargets) {
                focusedTargets = detail::makeAtSpiFocusedTargetProvider();
            }
            return std::make_unique<PortalScreenContext>(
                std::move(portal), std::move(focusedTargets));
        case SessionType::unknown:
            return std::make_unique<UnavailableScreenContext>(Status::failure(
                ErrorCode::unsupported_session,
                "Screen context requires an X11 or Wayland graphical session."));
    }
    return std::make_unique<UnavailableScreenContext>(Status::failure(
        ErrorCode::internal_error,
        "Unknown Linux session type."));
}

std::shared_ptr<FocusedTargetProvider> makeAtSpiFocusedTargetProvider(
    const CapabilityReport& report) {
    const auto* capability = report.find(Feature::accessibility_context);
    if (capability && capability->usable()) {
        return detail::makeAtSpiFocusedTargetProvider();
    }
    return std::make_shared<UnavailableFocusedTargetProvider>(Status::failure(
        ErrorCode::missing_dependency,
        capability
            ? capability->detail
            : "AT-SPI2 focused-target metadata was not detected.",
        capability ? capability->remediation : std::string{}));
}

}  // namespace localflow::platform::linux
