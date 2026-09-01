import Foundation
import CoreGraphics

/// Synthetic keyboard event posting. Requires Accessibility trust.
/// Events are posted to the HID event tap without activating any app, so the
/// frontmost app keeps focus.
enum KeyEvents {
    private static let vKeyCode: CGKeyCode = 9 // kVK_ANSI_V

    /// Posts Cmd-V (key down + up) to the HID event tap.
    static func postCommandV() throws {
        guard
            let source = CGEventSource(stateID: .combinedSessionState),
            let keyDown = CGEvent(keyboardEventSource: source, virtualKey: vKeyCode, keyDown: true),
            let keyUp = CGEvent(keyboardEventSource: source, virtualKey: vKeyCode, keyDown: false)
        else {
            throw InsertionError.eventCreationFailed
        }
        keyDown.flags = .maskCommand
        keyUp.flags = .maskCommand
        keyDown.post(tap: .cghidEventTap)
        usleep(20_000) // small down→up gap so the target registers the chord
        keyUp.post(tap: .cghidEventTap)
    }

    /// Types `text` via `keyboardSetUnicodeString`, in chunks small enough that
    /// event delivery stays reliable (~20 UTF-16 units per event).
    static func typeUnicode(_ text: String, chunkSize: Int, interChunkDelay: TimeInterval) throws {
        guard let source = CGEventSource(stateID: .combinedSessionState) else {
            throw InsertionError.eventCreationFailed
        }
        for chunk in TextChunker.chunks(text, maxUTF16PerChunk: chunkSize) {
            var units = Array(chunk.utf16)
            guard
                let keyDown = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: true),
                let keyUp = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: false)
            else {
                throw InsertionError.eventCreationFailed
            }
            keyDown.keyboardSetUnicodeString(stringLength: units.count, unicodeString: &units)
            keyUp.keyboardSetUnicodeString(stringLength: units.count, unicodeString: &units)
            keyDown.post(tap: .cghidEventTap)
            keyUp.post(tap: .cghidEventTap)
            if interChunkDelay > 0 {
                usleep(useconds_t(interChunkDelay * 1_000_000))
            }
        }
    }
}
