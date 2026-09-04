#pragma once

#include "localflow/linux/Capabilities.hpp"
#include "localflow/linux/ScreenContext.hpp"
#include "localflow/linux/Status.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace localflow::platform::linux {

class AccessibilityTextInserter {
public:
    virtual ~AccessibilityTextInserter() = default;

    // Inserts at the focused object's caret. Implementations must return
    // not_editable when focus is known but does not implement EditableText.
    virtual Status insertAtCaret(const std::string& utf8Text) = 0;

    // Production AT-SPI implementations revalidate the exact expected target
    // and insert through the same accessible object they just verified.
    virtual Status insertAtCaret(
        const std::string& utf8Text,
        const ApplicationInfo& expectedTarget) {
        (void)expectedTarget;
        return insertAtCaret(utf8Text);
    }
};

struct ClipboardSnapshot {
    // MIME type -> exact offered bytes, including zero-length payloads.
    std::map<std::string, std::vector<std::uint8_t>> payloads;

    // QMimeData can expose an image as a semantic QVariant without encoded
    // MIME bytes. The Qt adapter stores a lossless PNG copy here so that such
    // images survive the transaction as image data as well.
    std::vector<std::uint8_t> semanticImagePng;
};

class Clipboard {
public:
    virtual ~Clipboard() = default;

    [[nodiscard]] virtual Result<ClipboardSnapshot> snapshot() = 0;
    virtual Status setText(const std::string& utf8Text) = 0;

    // Must not overwrite a clipboard that the user or another application
    // replaced after setText(). A skipped restore is a successful outcome.
    virtual Status restore(const ClipboardSnapshot& snapshot) = 0;
};

class PasteInjector {
public:
    virtual ~PasteInjector() = default;
    virtual Status paste() = 0;
};

// Injectable boundary for an approved Wayland RemoteDesktop keyboard session.
// The default Qt/QDBus transport maps sendKeysym to NotifyKeyboardKeysym.
class RemoteDesktopPortal {
public:
    virtual ~RemoteDesktopPortal() = default;
    virtual Status ensureKeyboardSession() = 0;
    virtual Status sendKeysym(std::uint32_t keysym, bool pressed) = 0;
    virtual void close() noexcept = 0;
};

struct InsertionAttempt {
    std::string backend;
    Status status;
};

struct InsertionResult {
    Status status;
    std::string backend;
    std::vector<InsertionAttempt> attempts;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct InsertionOptions {
    bool allowClipboardFallback{true};
    std::chrono::milliseconds clipboardRestoreDelay{180};
    bool copyOnFocusChange{true};
};

class TextInsertionCoordinator {
public:
    TextInsertionCoordinator(
        std::unique_ptr<AccessibilityTextInserter> accessibility,
        std::unique_ptr<Clipboard> clipboard,
        std::unique_ptr<PasteInjector> paste,
        InsertionOptions options = {},
        std::shared_ptr<FocusedTargetProvider> focusedTargets = {});

    [[nodiscard]] InsertionResult insert(const std::string& utf8Text);
    [[nodiscard]] InsertionResult insert(
        const std::string& utf8Text,
        const ApplicationInfo& expectedTarget);

private:
    [[nodiscard]] InsertionResult insertImpl(
        const std::string& utf8Text,
        const ApplicationInfo* expectedTarget);

    std::unique_ptr<AccessibilityTextInserter> accessibility_;
    std::unique_ptr<Clipboard> clipboard_;
    std::unique_ptr<PasteInjector> paste_;
    InsertionOptions options_;
    std::shared_ptr<FocusedTargetProvider> focusedTargets_;
};

[[nodiscard]] std::unique_ptr<AccessibilityTextInserter>
makeAtSpiTextInserter(const CapabilityReport& report);

[[nodiscard]] std::unique_ptr<Clipboard> makeSystemClipboard(
    const CapabilityReport& report);

[[nodiscard]] std::unique_ptr<PasteInjector> makePasteInjector(
    const CapabilityReport& report,
    std::shared_ptr<RemoteDesktopPortal> remoteDesktop = {});

}  // namespace localflow::platform::linux
