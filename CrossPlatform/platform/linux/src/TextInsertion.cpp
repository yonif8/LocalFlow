#include "localflow/linux/TextInsertion.hpp"

#include "InternalFactories.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace localflow::platform::linux {
namespace {

class UnavailableAccessibilityInserter final : public AccessibilityTextInserter {
public:
    explicit UnavailableAccessibilityInserter(Status status) : status_(std::move(status)) {}
    Status insertAtCaret(const std::string&) override { return status_; }

private:
    Status status_;
};

class UnavailableClipboard final : public Clipboard {
public:
    explicit UnavailableClipboard(Status status) : status_(std::move(status)) {}
    Result<ClipboardSnapshot> snapshot() override {
        return Result<ClipboardSnapshot>::failure(status_);
    }
    Status setText(const std::string&) override { return status_; }
    Status restore(const ClipboardSnapshot&) override { return status_; }

private:
    Status status_;
};

class UnavailablePasteInjector final : public PasteInjector {
public:
    explicit UnavailablePasteInjector(Status status) : status_(std::move(status)) {}
    Status paste() override { return status_; }

private:
    Status status_;
};

class PortalPasteInjector final : public PasteInjector {
public:
    explicit PortalPasteInjector(std::shared_ptr<RemoteDesktopPortal> portal)
        : portal_(std::move(portal)) {}

    ~PortalPasteInjector() override {
        if (portal_) {
            portal_->close();
        }
    }

    Status paste() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!portal_) {
            return Status::failure(
                ErrorCode::not_configured,
                "The Wayland RemoteDesktop keyboard transport was not configured.",
                "Install Qt DBus support and xdg-desktop-portal before using clipboard paste fallback.");
        }
        auto status = portal_->ensureKeyboardSession();
        if (!status.ok()) {
            return status;
        }

        // XKB keysyms used by NotifyKeyboardKeysym: Control_L and lowercase v.
        constexpr std::uint32_t controlLeft = 0xffe3;
        constexpr std::uint32_t letterV = 0x0076;
        status = portal_->sendKeysym(controlLeft, true);
        if (!status.ok()) return status;
        status = portal_->sendKeysym(letterV, true);
        if (!status.ok()) {
            (void)portal_->sendKeysym(controlLeft, false);
            return status;
        }
        const auto releaseV = portal_->sendKeysym(letterV, false);
        const auto releaseControl = portal_->sendKeysym(controlLeft, false);
        if (!releaseV.ok()) return releaseV;
        return releaseControl;
    }

private:
    std::shared_ptr<RemoteDesktopPortal> portal_;
    std::mutex mutex_;
};

Status capabilityFailure(
    const CapabilityReport& report,
    Feature feature,
    std::string fallbackDetail) {
    const auto* capability = report.find(feature);
    return Status::failure(
        ErrorCode::missing_dependency,
        capability ? capability->detail : std::move(fallbackDetail),
        capability ? capability->remediation : std::string{});
}

}  // namespace

TextInsertionCoordinator::TextInsertionCoordinator(
    std::unique_ptr<AccessibilityTextInserter> accessibility,
    std::unique_ptr<Clipboard> clipboard,
    std::unique_ptr<PasteInjector> paste,
    InsertionOptions options)
    : accessibility_(std::move(accessibility)),
      clipboard_(std::move(clipboard)),
      paste_(std::move(paste)),
      options_(options) {}

InsertionResult TextInsertionCoordinator::insert(const std::string& utf8Text) {
    InsertionResult result;
    if (utf8Text.empty()) {
        result.status = Status::failure(
            ErrorCode::invalid_argument,
            "LocalFlow will not insert an empty transcript.");
        return result;
    }

    if (accessibility_) {
        auto status = accessibility_->insertAtCaret(utf8Text);
        result.attempts.push_back({"AT-SPI2 EditableText", status});
        if (status.ok()) {
            result.status = Status::success();
            result.backend = "AT-SPI2 EditableText";
            return result;
        }
    }

    if (!options_.allowClipboardFallback) {
        result.status = result.attempts.empty()
            ? Status::failure(ErrorCode::not_configured, "No text insertion backend is configured.")
            : result.attempts.back().status;
        return result;
    }
    if (!clipboard_ || !paste_) {
        result.status = Status::failure(
            ErrorCode::not_configured,
            "Clipboard fallback requires both clipboard and paste adapters.");
        return result;
    }

    const auto snapshot = clipboard_->snapshot();
    result.attempts.push_back({"clipboard snapshot", snapshot.status()});
    if (!snapshot) {
        result.status = snapshot.status();
        return result;
    }

    auto setStatus = clipboard_->setText(utf8Text);
    result.attempts.push_back({"clipboard write", setStatus});
    if (!setStatus.ok()) {
        result.status = setStatus;
        return result;
    }

    auto pasteStatus = paste_->paste();
    result.attempts.push_back({"clipboard paste", pasteStatus});

    // Most applications consume the selection asynchronously. Restoring too
    // early intermittently pastes the user's old clipboard instead.
    if (pasteStatus.ok() && options_.clipboardRestoreDelay.count() > 0) {
        std::this_thread::sleep_for(options_.clipboardRestoreDelay);
    }
    const auto restoreStatus = clipboard_->restore(snapshot.value());
    result.attempts.push_back({"clipboard restore", restoreStatus});

    if (!pasteStatus.ok()) {
        result.status = pasteStatus;
        return result;
    }
    if (!restoreStatus.ok()) {
        result.status = Status::failure(
            ErrorCode::io_error,
            "The transcript was pasted, but the previous clipboard could not be restored: " +
                restoreStatus.message,
            restoreStatus.remediation);
        result.backend = "clipboard paste";
        return result;
    }

    result.status = Status::success();
    result.backend = "clipboard paste";
    return result;
}

std::unique_ptr<AccessibilityTextInserter> makeAtSpiTextInserter(
    const CapabilityReport& report) {
    const auto* capability = report.find(Feature::accessibility_insertion);
    if (capability && capability->usable()) {
        return detail::makeAtSpiInserter();
    }
    return std::make_unique<UnavailableAccessibilityInserter>(capabilityFailure(
        report,
        Feature::accessibility_insertion,
        "AT-SPI2 accessibility insertion was not detected."));
}

std::unique_ptr<Clipboard> makeSystemClipboard(const CapabilityReport& report) {
    const auto* capability = report.find(Feature::clipboard_paste);
    if (!capability || !capability->usable()) {
        return std::make_unique<UnavailableClipboard>(capabilityFailure(
            report,
            Feature::clipboard_paste,
            "Clipboard paste support was not detected."));
    }
    if (auto clipboard = detail::makeQtClipboard()) {
        return clipboard;
    }
    return detail::makeCommandClipboard(report.session.type, capability->backend);
}

std::unique_ptr<PasteInjector> makePasteInjector(
    const CapabilityReport& report,
    std::shared_ptr<RemoteDesktopPortal> remoteDesktop) {
    const auto* capability = report.find(Feature::clipboard_paste);
    if (!capability || !capability->usable()) {
        return std::make_unique<UnavailablePasteInjector>(capabilityFailure(
            report,
            Feature::clipboard_paste,
            "Paste injection support was not detected."));
    }
    switch (report.session.type) {
        case SessionType::x11:
            return detail::makeX11PasteInjector();
        case SessionType::wayland:
            if (!remoteDesktop) {
                remoteDesktop = detail::makeQDbusRemoteDesktopPortal();
            }
            return std::make_unique<PortalPasteInjector>(std::move(remoteDesktop));
        case SessionType::unknown:
            break;
    }
    return std::make_unique<UnavailablePasteInjector>(Status::failure(
        ErrorCode::unsupported_session,
        "Paste injection requires X11 or an approved Wayland RemoteDesktop session."));
}

}  // namespace localflow::platform::linux
