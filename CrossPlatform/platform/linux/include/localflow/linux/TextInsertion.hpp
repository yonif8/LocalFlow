#pragma once

#include "localflow/linux/Capabilities.hpp"
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
};

struct ClipboardSnapshot {
    // MIME type -> exact bytes. Production clipboard implementations should
    // preserve all offered types, not only text/plain.
    std::map<std::string, std::vector<std::uint8_t>> payloads;
};

class Clipboard {
public:
    virtual ~Clipboard() = default;

    [[nodiscard]] virtual Result<ClipboardSnapshot> snapshot() = 0;
    virtual Status setText(const std::string& utf8Text) = 0;
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
};

class TextInsertionCoordinator {
public:
    TextInsertionCoordinator(
        std::unique_ptr<AccessibilityTextInserter> accessibility,
        std::unique_ptr<Clipboard> clipboard,
        std::unique_ptr<PasteInjector> paste,
        InsertionOptions options = {});

    [[nodiscard]] InsertionResult insert(const std::string& utf8Text);

private:
    std::unique_ptr<AccessibilityTextInserter> accessibility_;
    std::unique_ptr<Clipboard> clipboard_;
    std::unique_ptr<PasteInjector> paste_;
    InsertionOptions options_;
};

[[nodiscard]] std::unique_ptr<AccessibilityTextInserter>
makeAtSpiTextInserter(const CapabilityReport& report);

[[nodiscard]] std::unique_ptr<Clipboard> makeSystemClipboard(
    const CapabilityReport& report);

[[nodiscard]] std::unique_ptr<PasteInjector> makePasteInjector(
    const CapabilityReport& report,
    std::shared_ptr<RemoteDesktopPortal> remoteDesktop = {});

}  // namespace localflow::platform::linux
