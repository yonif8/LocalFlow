import AppKit
import ApplicationServices

/// Info about the focused AX element, for diagnostics.
public struct FocusedElementInfo: Sendable {
    public let role: String?
    public let subrole: String?
    public let selectedTextSettable: Bool
    /// Non-nil if the focused element could not be obtained.
    public let lookupError: AXError?
}

/// Strategy 1: insert by setting `kAXSelectedTextAttribute` on the focused
/// element of the system-wide accessibility object. Replaces the current
/// selection, or inserts at the caret when the selection is empty.
@MainActor
enum AXInsertion {
    static func focusedElement() throws -> AXUIElement {
        // Primary: focused element of the system-wide accessibility object.
        let systemWide = AXUIElementCreateSystemWide()
        var focused: CFTypeRef?
        let err = AXUIElementCopyAttributeValue(
            systemWide, kAXFocusedUIElementAttribute as CFString, &focused
        )
        if err == .success, let ref = focused, CFGetTypeID(ref) == AXUIElementGetTypeID() {
            return (ref as! AXUIElement)
        }

        // Fallback: the system-wide query can fail with kAXErrorCannotComplete
        // (-25204) in some launch contexts even when per-app queries work.
        // Ask the frontmost application's own AX element for its focused element.
        let debug = ProcessInfo.processInfo.environment["LFINSERT_DEBUG"] != nil
        if debug {
            FileHandle.standardError.write(Data("[lfinsert] systemwide focused query err=\(err.rawValue)\n".utf8))
        }
        if let frontmost = NSWorkspace.shared.frontmostApplication {
            let appElement = AXUIElementCreateApplication(frontmost.processIdentifier)
            var appFocused: CFTypeRef?
            let appErr = AXUIElementCopyAttributeValue(
                appElement, kAXFocusedUIElementAttribute as CFString, &appFocused
            )
            if debug {
                FileHandle.standardError.write(Data("[lfinsert] fallback app=\(frontmost.bundleIdentifier ?? "?") pid=\(frontmost.processIdentifier) err=\(appErr.rawValue)\n".utf8))
            }
            if appErr == .success, let ref = appFocused, CFGetTypeID(ref) == AXUIElementGetTypeID() {
                return (ref as! AXUIElement)
            }
        } else if debug {
            FileHandle.standardError.write(Data("[lfinsert] fallback: no frontmost application\n".utf8))
        }
        throw InsertionError.noFocusedElement(err)
    }

    /// Attempts the AX insertion. Throws on any failure so the caller can fall
    /// through to the next strategy.
    static func insert(_ text: String, verify: Bool) throws {
        let element = try focusedElement()

        // Check settability BEFORE writing: some apps (WhatsApp/Catalyst)
        // return .success for a write they silently ignore, and their value
        // attribute is unreadable — so the post-write verification cannot
        // catch the lie and the paste fallback never runs.
        var settable = DarwinBoolean(false)
        let settableErr = AXUIElementIsAttributeSettable(
            element, kAXSelectedTextAttribute as CFString, &settable
        )
        guard settableErr == .success, settable.boolValue else {
            throw InsertionError.axNotSettable
        }

        let setErr = AXUIElementSetAttributeValue(
            element, kAXSelectedTextAttribute as CFString, text as CFString
        )
        guard setErr == .success else {
            throw InsertionError.axSetFailed(setErr)
        }

        guard verify else { return }
        // Some apps (notably Electron) return success without inserting.
        // If the element exposes a readable string value, require our text in it.
        var valueRef: CFTypeRef?
        let valueErr = AXUIElementCopyAttributeValue(
            element, kAXValueAttribute as CFString, &valueRef
        )
        if valueErr == .success, let value = valueRef as? String {
            guard value.contains(text) else {
                throw InsertionError.axVerificationFailed
            }
        }
        // Value unreadable (secure field, no value attribute, huge doc refusal):
        // trust the .success error code.
    }

    /// Diagnostics for the doctor report; never throws.
    static func focusedElementInfo() -> FocusedElementInfo {
        let element: AXUIElement
        do {
            element = try focusedElement()
        } catch {
            let axErr = (error as? InsertionError).flatMap { e -> AXError? in
                if case .noFocusedElement(let code) = e { return code }
                return nil
            }
            return FocusedElementInfo(
                role: nil, subrole: nil, selectedTextSettable: false, lookupError: axErr
            )
        }

        func stringAttr(_ name: String) -> String? {
            var ref: CFTypeRef?
            guard AXUIElementCopyAttributeValue(element, name as CFString, &ref) == .success else {
                return nil
            }
            return ref as? String
        }

        var settable = DarwinBoolean(false)
        let settableErr = AXUIElementIsAttributeSettable(
            element, kAXSelectedTextAttribute as CFString, &settable
        )
        return FocusedElementInfo(
            role: stringAttr(kAXRoleAttribute as String),
            subrole: stringAttr(kAXSubroleAttribute as String),
            selectedTextSettable: settableErr == .success && settable.boolValue,
            lookupError: nil
        )
    }
}
