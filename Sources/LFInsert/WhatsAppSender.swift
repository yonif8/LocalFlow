import AppKit
import ApplicationServices

// MARK: - Pure policy (unit-tested)

/// Normalizes a user-typed WhatsApp recipient ("+61 412 345 678",
/// "0061-412…") into the digits-only international form WhatsApp's
/// `send?phone=` deep link expects. Returns nil unless it looks like an
/// E.164 number (7–15 digits, country code included).
public enum WhatsAppRecipient {
    public static func normalize(_ input: String) -> String? {
        let trimmed = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        // Only digits and common separators are allowed: letters mean the
        // user typed a name, which the deep link cannot resolve.
        let allowed = CharacterSet(charactersIn: "0123456789+-() .")
        guard trimmed.unicodeScalars.allSatisfy(allowed.contains) else { return nil }
        var digits = trimmed.filter(\.isASCII).filter(\.isNumber)
        if !trimmed.hasPrefix("+"), digits.hasPrefix("00") { digits.removeFirst(2) }
        guard (7...15).contains(digits.count), digits.first != "0" else { return nil }
        return digits
    }
}

/// Decides whether a focused Accessibility element is somewhere the user can
/// type. Settability alone is not enough (some web/Electron fields report it
/// unsettable), so text roles count as editable too. No focus at all, or a
/// focus that is clearly not a text input, means "nowhere to type".
public enum EditableFocusPolicy {
    static let textRoles: Set<String> = [
        "AXTextField", "AXTextArea", "AXComboBox", "AXSearchField", "AXSecureTextField",
    ]

    public static func isEditable(role: String?, subrole: String?, selectedTextSettable: Bool) -> Bool {
        if selectedTextSettable { return true }
        if let role, textRoles.contains(role) { return true }
        if let subrole, subrole == "AXSecureTextField" || subrole == "AXSearchField" { return true }
        return false
    }

    /// Live check of the system's current keyboard focus.
    @MainActor
    public static func hasEditableFocus() -> Bool {
        let info = AXInsertion.focusedElementInfo()
        return isEditable(role: info.role, subrole: info.subrole,
                          selectedTextSettable: info.selectedTextSettable)
    }
}

/// Reads the item count WhatsApp announces in its media preview
/// ("1 media item.", "3 media items."). Nil when no number is present.
public enum WhatsAppMediaCount {
    public static func parse(_ label: String) -> Int? {
        let digits = label.split(whereSeparator: { !$0.isNumber })
        guard digits.count == 1, let value = Int(digits[0]) else { return nil }
        return value
    }
}

// MARK: - Sender

public enum WhatsAppSendError: Error, CustomStringConvertible, Equatable {
    case accessibilityNotGranted
    case notInstalled
    case invalidRecipient
    case imageEncodingFailed
    case windowUnavailable
    case chatDidNotOpen
    case previewDidNotOpen
    case unexpectedMediaCount(Int)
    case captionNotAccepted
    case sendNotConfirmed

    public var description: String {
        switch self {
        case .accessibilityNotGranted: return "Accessibility permission is needed to send via WhatsApp."
        case .notInstalled: return "WhatsApp isn't installed."
        case .invalidRecipient: return "Set a WhatsApp number in Settings → Dictation."
        case .imageEncodingFailed: return "Couldn't prepare the screenshot."
        case .windowUnavailable: return "Couldn't reach WhatsApp's window."
        case .chatDidNotOpen: return "WhatsApp didn't open that chat. Check the number."
        case .previewDidNotOpen: return "WhatsApp didn't accept the screenshot."
        case .unexpectedMediaCount(let n): return "WhatsApp staged \(n) images; cancelled."
        case .captionNotAccepted: return "WhatsApp didn't accept the caption."
        case .sendNotConfirmed: return "WhatsApp didn't confirm the send."
        }
    }
}

/// Sends one WhatsApp message — a PNG image with a text caption — through
/// the installed WhatsApp for Mac (Catalyst) app WITHOUT bringing it to the
/// front. Proven mechanism (WhatsApp 26.46):
///
/// - A *hidden* Catalyst app ignores AXPress, so it is unhidden without
///   activation (its window sits behind the user's app) and re-hidden after.
/// - `whatsapp://send?phone=` opened with `activates = false` switches chats
///   without activating. A neutral pane is shown first so a compose box that
///   appears afterwards can only belong to the linked chat.
/// - Edit → Paste is disabled while inactive, so Cmd-V is posted straight to
///   the process. It MUST be the full ⌘↓ V↓ V↑ ⌘↑ sequence: without the
///   modifier key-up UIKit's own key-repeat pastes the image dozens of times.
/// - The media preview's caption field and Send button are plain AX elements.
///
/// UI labels are matched in English; other WhatsApp languages fail safely
/// (nothing is sent and the caller keeps the transcript).
@MainActor
public final class WhatsAppSender {
    public static let bundleID = "net.whatsapp.WhatsApp"

    private let pasteboard: NSPasteboard

    public init(pasteboard: NSPasteboard = .general) {
        self.pasteboard = pasteboard
    }

    public static var isInstalled: Bool {
        NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleID) != nil
    }

    public func send(png: Data, caption: String, to rawRecipient: String) async throws {
        guard AXIsProcessTrusted() else { throw WhatsAppSendError.accessibilityNotGranted }
        guard let phone = WhatsAppRecipient.normalize(rawRecipient) else {
            throw WhatsAppSendError.invalidRecipient
        }
        guard let appURL = NSWorkspace.shared.urlForApplication(withBundleIdentifier: Self.bundleID) else {
            throw WhatsAppSendError.notInstalled
        }
        guard !png.isEmpty else { throw WhatsAppSendError.imageEncodingFailed }

        let (app, shouldRehide) = try await prepareApp(at: appURL)
        defer { if shouldRehide { app.hide() } }
        let ax = AXUIElementCreateApplication(app.processIdentifier)

        guard try await waitFor(seconds: 10, { self.mainWindow(ax) != nil }) else {
            throw WhatsAppSendError.windowUnavailable
        }
        // Leave any half-finished media preview so we start from a clean state.
        if let cancel = element(in: ax, role: "AXButton", label: "Cancel"),
           element(in: ax, role: "AXTextArea", label: "Caption text field") != nil {
            press(cancel)
            try await Task.sleep(for: .milliseconds(400))
        }

        // Neutral pane, then the deep link: a compose box appearing after
        // this point is the linked chat, never a previously open one.
        if let settings = element(in: ax, role: "AXButton", label: "\u{200E}You", exact: true)
            ?? element(in: ax, role: "AXButton", label: "You", exact: true) {
            press(settings)
            _ = try await waitFor(seconds: 2) { self.composeField(ax) == nil }
        }
        guard composeField(ax) == nil else { throw WhatsAppSendError.chatDidNotOpen }
        try await openChat(phone: phone, appURL: appURL)
        guard try await waitFor(seconds: 6, { self.composeField(ax) != nil }),
              let compose = composeField(ax) else {
            throw WhatsAppSendError.chatDidNotOpen
        }
        AXUIElementSetAttributeValue(compose, kAXFocusedAttribute as CFString, kCFBooleanTrue)
        try await Task.sleep(for: .milliseconds(250))

        // Stage exactly one PNG on the clipboard, paste, then restore.
        let saved = PasteboardTransfer.snapshot(of: pasteboard)
        pasteboard.clearContents()
        let item = NSPasteboardItem()
        item.setData(png, forType: .png)
        pasteboard.writeObjects([item])
        let stagedChangeCount = pasteboard.changeCount
        defer {
            PasteboardTransfer.restore(saved, to: pasteboard,
                                       ifChangeCountStillEquals: stagedChangeCount)
        }
        try postCommandV(to: app.processIdentifier)

        guard try await waitFor(seconds: 6, { self.captionField(ax) != nil }),
              let captionElement = captionField(ax) else {
            throw WhatsAppSendError.previewDidNotOpen
        }
        do {
            // Let a runaway paste (should one ever happen) show its count.
            try await Task.sleep(for: .milliseconds(500))
            if let count = mediaCount(ax), count != 1 {
                throw WhatsAppSendError.unexpectedMediaCount(count)
            }
            let text = Self.trimmedCaption(caption)
            guard AXUIElementSetAttributeValue(captionElement, kAXValueAttribute as CFString,
                                               text as CFString) == .success else {
                throw WhatsAppSendError.captionNotAccepted
            }
            try await Task.sleep(for: .milliseconds(200))
            if !text.isEmpty, (stringAttribute(captionElement, kAXValueAttribute) ?? "").isEmpty {
                throw WhatsAppSendError.captionNotAccepted
            }
            guard let sendButton = element(in: ax, role: "AXButton", label: "Send", exact: true)
                ?? element(in: ax, role: "AXButton", label: "\u{200E}Send", exact: true) else {
                throw WhatsAppSendError.sendNotConfirmed
            }
            press(sendButton)
        } catch {
            if let cancel = element(in: ax, role: "AXButton", label: "Cancel") { press(cancel) }
            throw error
        }
        guard try await waitFor(seconds: 8, { self.captionField(ax) == nil }) else {
            throw WhatsAppSendError.sendNotConfirmed
        }
    }

    /// WhatsApp caps captions at 1024 characters.
    static func trimmedCaption(_ text: String) -> String {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.count > 1024 ? String(trimmed.prefix(1023)) + "…" : trimmed
    }

    // MARK: App lifecycle

    private func prepareApp(at appURL: URL) async throws -> (NSRunningApplication, Bool) {
        if let running = NSRunningApplication.runningApplications(withBundleIdentifier: Self.bundleID).first {
            let wasHidden = running.isHidden
            if wasHidden {
                running.unhide()
                try await Task.sleep(for: .milliseconds(400))
            }
            return (running, wasHidden)
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = false
        configuration.addsToRecentItems = false
        let app = try await NSWorkspace.shared.openApplication(at: appURL, configuration: configuration)
        // A freshly launched app needs a moment before its UI responds.
        try await Task.sleep(for: .seconds(2))
        return (app, true)
    }

    private func openChat(phone: String, appURL: URL) async throws {
        guard let url = URL(string: "whatsapp://send?phone=\(phone)") else {
            throw WhatsAppSendError.invalidRecipient
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = false
        configuration.addsToRecentItems = false
        _ = try await NSWorkspace.shared.open([url], withApplicationAt: appURL,
                                              configuration: configuration)
    }

    /// ⌘↓ V↓ V↑ ⌘↑ delivered to the process only. The trailing key-ups are
    /// what stop UIKit's key-repeat; never post V with only a flag mask.
    private func postCommandV(to pid: pid_t) throws {
        guard let source = CGEventSource(stateID: .hidSystemState) else {
            throw InsertionError.eventCreationFailed
        }
        let sequence: [(CGKeyCode, Bool, CGEventFlags)] = [
            (55, true, .maskCommand), (9, true, .maskCommand),
            (9, false, .maskCommand), (55, false, []),
        ]
        for (key, down, flags) in sequence {
            guard let event = CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: down) else {
                throw InsertionError.eventCreationFailed
            }
            event.flags = flags
            event.postToPid(pid)
            usleep(20_000)
        }
    }

    // MARK: Accessibility helpers

    private func waitFor(seconds: Double, _ condition: @MainActor () -> Bool) async throws -> Bool {
        let deadline = ContinuousClock.now + .milliseconds(Int(seconds * 1000))
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try await Task.sleep(for: .milliseconds(100))
        }
        return condition()
    }

    private func mainWindow(_ ax: AXUIElement) -> AXUIElement? {
        (attribute(ax, kAXWindowsAttribute) as? [AXUIElement])?.first
    }

    private func composeField(_ ax: AXUIElement) -> AXUIElement? {
        element(in: ax, role: "AXTextArea", label: "Compose message")
    }

    private func captionField(_ ax: AXUIElement) -> AXUIElement? {
        element(in: ax, role: "AXTextArea", label: "Caption text field")
    }

    private func mediaCount(_ ax: AXUIElement) -> Int? {
        element(in: ax, role: "AXGenericElement", label: "media item")
            .flatMap { label(of: $0) }
            .flatMap(WhatsAppMediaCount.parse)
    }

    private func element(in ax: AXUIElement, role: String, label wanted: String,
                         exact: Bool = false) -> AXUIElement? {
        guard let window = mainWindow(ax) else { return nil }
        var visited = 0
        func search(_ node: AXUIElement, depth: Int) -> AXUIElement? {
            visited += 1
            guard depth < 18, visited < 4000 else { return nil }
            if stringAttribute(node, kAXRoleAttribute) == role, let text = label(of: node) {
                let clean = text.replacingOccurrences(of: "\u{200E}", with: "")
                let target = wanted.replacingOccurrences(of: "\u{200E}", with: "")
                if exact ? clean == target : clean.contains(target) { return node }
            }
            for child in (attribute(node, kAXChildrenAttribute) as? [AXUIElement]) ?? [] {
                if let hit = search(child, depth: depth + 1) { return hit }
            }
            return nil
        }
        return search(window, depth: 0)
    }

    private func label(of node: AXUIElement) -> String? {
        for name in [kAXDescriptionAttribute, kAXTitleAttribute] {
            if let value = stringAttribute(node, name), !value.isEmpty { return value }
        }
        return nil
    }

    private func press(_ node: AXUIElement) {
        AXUIElementPerformAction(node, kAXPressAction as CFString)
    }

    private func attribute(_ node: AXUIElement, _ name: String) -> CFTypeRef? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(node, name as CFString, &value) == .success else { return nil }
        return value
    }

    private func stringAttribute(_ node: AXUIElement, _ name: String) -> String? {
        attribute(node, name) as? String
    }
}
