#include "localflow/linux/GlobalShortcut.hpp"

#include "InternalFactories.hpp"

#include <mutex>
#include <utility>

namespace localflow::platform::linux {
namespace {

class UnavailableShortcutBackend final : public GlobalShortcutBackend {
public:
    explicit UnavailableShortcutBackend(Status status)
        : status_(std::move(status)) {}

    std::string name() const override { return "unavailable"; }

    Status start(const ShortcutSpec&, ShortcutCallback) override { return status_; }
    void stop() noexcept override {}

private:
    Status status_;
};

class PortalShortcutBackend final : public GlobalShortcutBackend {
public:
    explicit PortalShortcutBackend(std::shared_ptr<GlobalShortcutsPortal> portal)
        : portal_(std::move(portal)) {}

    ~PortalShortcutBackend() override { stop(); }

    std::string name() const override { return "xdg-desktop-portal GlobalShortcuts"; }

    Status start(const ShortcutSpec& shortcut, ShortcutCallback callback) override {
        if (!portal_) {
            return Status::failure(
                ErrorCode::not_configured,
                "The GlobalShortcuts portal transport was not configured.",
                "Install Qt DBus support and xdg-desktop-portal before enabling push-to-talk.");
        }
        if (shortcut.kind == ShortcutKind::mouse_button) {
            return Status::failure(
                ErrorCode::unsupported_session,
                "The standard Wayland GlobalShortcuts portal does not support arbitrary mouse buttons.",
                "Choose a keyboard shortcut, or use an X11 session for a side-mouse-button trigger.");
        }
        if (shortcut.id.empty() || shortcut.trigger.empty() || !callback) {
            return Status::failure(
                ErrorCode::invalid_argument,
                "A shortcut id, trigger, and callback are required.");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != State::idle) {
                return Status::failure(
                    ErrorCode::busy,
                    "A global shortcut session is already active or changing state.");
            }
            state_ = State::starting;
        }
        const auto status = portal_->bind(shortcut, std::move(callback));
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::starting) {
            if (state_ == State::stopping) state_ = State::idle;
            return Status::failure(
                ErrorCode::cancelled,
                "Global shortcut setup was cancelled while waiting for desktop consent.");
        }
        state_ = status.ok() ? State::running : State::idle;
        return status;
    }

    void stop() noexcept override {
        bool setupInFlight = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!portal_ || state_ == State::idle || state_ == State::stopping) {
                return;
            }
            setupInFlight = state_ == State::starting;
            state_ = State::stopping;
        }
        // close() cancels an in-flight portal Request. Do not hold mutex_ here:
        // bind() must be able to return and observe the stopping state.
        portal_->close();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!setupInFlight || state_ != State::stopping) {
            state_ = State::idle;
        }
    }

private:
    enum class State {
        idle,
        starting,
        running,
        stopping,
    };

    std::shared_ptr<GlobalShortcutsPortal> portal_;
    std::mutex mutex_;
    State state_{State::idle};
};

}  // namespace

std::unique_ptr<GlobalShortcutBackend> makeGlobalShortcutBackend(
    const CapabilityReport& report,
    std::shared_ptr<GlobalShortcutsPortal> portal) {
    const auto* capability = report.find(Feature::global_shortcut);
    if (capability == nullptr || !capability->usable()) {
        return std::make_unique<UnavailableShortcutBackend>(Status::failure(
            ErrorCode::unsupported_session,
            capability ? capability->detail : "Global shortcut capability was not probed.",
            capability ? capability->remediation : "Run Linux capability detection before constructing adapters."));
    }

    switch (report.session.type) {
        case SessionType::x11:
            return detail::makeX11ShortcutBackend();
        case SessionType::wayland:
            if (!portal) portal = detail::makeQDbusGlobalShortcutsPortal();
            return std::make_unique<PortalShortcutBackend>(std::move(portal));
        case SessionType::unknown:
            break;
    }

    return std::make_unique<UnavailableShortcutBackend>(Status::failure(
        ErrorCode::unsupported_session,
        "Global shortcuts require X11 or a Wayland GlobalShortcuts portal."));
}

}  // namespace localflow::platform::linux
