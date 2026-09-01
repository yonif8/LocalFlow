import Foundation
import ApplicationServices
import Carbon.HIToolbox

/// Permission and secure-input checks needed before attempting insertion.
public enum InsertPermissions {
    /// Whether this process is trusted for Accessibility.
    /// Both AX attribute setting and synthetic CGEvent posting require this.
    /// - Parameter promptIfNeeded: if true and not trusted, shows the system
    ///   prompt that adds this process to the Accessibility pane.
    public static func accessibilityGranted(promptIfNeeded: Bool = false) -> Bool {
        if promptIfNeeded {
            // Literal key instead of the kAXTrustedCheckOptionPrompt global:
            // the C global is a mutable CFStringRef and trips Swift 6 strict
            // concurrency. The key's value is ABI-stable.
            let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
            return AXIsProcessTrustedWithOptions(options)
        }
        return AXIsProcessTrusted()
    }

    /// Whether some process has enabled Secure Keyboard Entry (password fields,
    /// Terminal's "Secure Keyboard Entry" menu item, some password managers).
    /// While enabled, synthetic keyboard events may be ignored by the target.
    public static var secureInputEnabled: Bool {
        IsSecureEventInputEnabled()
    }
}
