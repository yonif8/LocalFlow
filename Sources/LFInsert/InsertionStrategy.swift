import Foundation
import ApplicationServices

/// Which mechanism was used (or attempted) to insert text into the frontmost app.
public enum InsertionStrategy: String, Sendable, CaseIterable, CustomStringConvertible {
    /// Set `kAXSelectedTextAttribute` on the focused AX element (inserts at caret / replaces selection).
    case ax
    /// Save pasteboard, write transient string, synthesize Cmd-V, restore pasteboard.
    case paste
    /// CGEvent `keyboardSetUnicodeString` typing in small chunks. Last resort; disabled by default.
    case type

    public var description: String {
        switch self {
        case .ax: return "ax (AXSelectedText)"
        case .paste: return "paste (pasteboard + Cmd-V)"
        case .type: return "type (synthetic unicode keystrokes)"
        }
    }
}

/// Tunables for `FrontmostInserter`. The strategy order is configurable;
/// `.type` is implemented but excluded from the default chain.
public struct InserterConfiguration: Sendable {
    /// Strategies tried in order until one succeeds.
    public var strategyOrder: [InsertionStrategy]
    /// How long to wait after posting Cmd-V before restoring the saved pasteboard.
    /// Too short races slow pasters (Electron apps); 300 ms is a safe default.
    public var pasteboardRestoreDelay: TimeInterval
    /// Max UTF-16 code units per synthetic-typing CGEvent.
    public var typingChunkSize: Int
    /// Delay between synthetic-typing chunks.
    public var typingInterChunkDelay: TimeInterval
    /// After a successful AX set, read back `kAXValue` and require the inserted
    /// text to appear in it (catches apps — notably Electron — that report
    /// success without inserting). If the value is unreadable, the AX error
    /// code alone is trusted.
    public var verifyAXInsertion: Bool

    public static let `default` = InserterConfiguration()

    public init(
        strategyOrder: [InsertionStrategy] = [.ax, .paste],
        pasteboardRestoreDelay: TimeInterval = 0.3,
        typingChunkSize: Int = 20,
        typingInterChunkDelay: TimeInterval = 0.005,
        verifyAXInsertion: Bool = true
    ) {
        self.strategyOrder = strategyOrder
        self.pasteboardRestoreDelay = pasteboardRestoreDelay
        self.typingChunkSize = max(1, typingChunkSize)
        self.typingInterChunkDelay = typingInterChunkDelay
        self.verifyAXInsertion = verifyAXInsertion
    }
}

/// One strategy's failure, kept for logging/HUD when later strategies run.
public struct StrategyFailure: Sendable, CustomStringConvertible {
    public let strategy: InsertionStrategy
    public let reason: String

    public init(strategy: InsertionStrategy, reason: String) {
        self.strategy = strategy
        self.reason = reason
    }

    public var description: String { "\(strategy.rawValue): \(reason)" }
}

/// Result of a successful insertion, for logging/HUD.
public struct InsertionOutcome: Sendable {
    /// The strategy that ultimately succeeded.
    public let strategy: InsertionStrategy
    /// Whether Secure Keyboard Entry was on at insertion time (worth surfacing in a HUD).
    public let secureInputWasEnabled: Bool
    /// Failures from strategies tried before the successful one.
    public let earlierFailures: [StrategyFailure]

    public init(strategy: InsertionStrategy, secureInputWasEnabled: Bool, earlierFailures: [StrategyFailure]) {
        self.strategy = strategy
        self.secureInputWasEnabled = secureInputWasEnabled
        self.earlierFailures = earlierFailures
    }
}

public enum InsertionError: Error, CustomStringConvertible {
    case emptyText
    case accessibilityNotGranted
    case noFocusedElement(AXError)
    case axNotSettable
    case axSetFailed(AXError)
    case axVerificationFailed
    case eventCreationFailed
    case allStrategiesFailed(failures: [StrategyFailure], secureInputWasEnabled: Bool)

    public var description: String {
        switch self {
        case .emptyText:
            return "Nothing to insert (empty text)."
        case .accessibilityNotGranted:
            return "Accessibility permission not granted. Enable it in System Settings > Privacy & Security > Accessibility for the app running this process."
        case .noFocusedElement(let err):
            return "No focused UI element found via Accessibility (AXError \(err.rawValue))."
        case .axNotSettable:
            return "Focused element does not support setting AXSelectedText."
        case .axSetFailed(let err):
            return "Setting AXSelectedText failed (AXError \(err.rawValue))."
        case .axVerificationFailed:
            return "App reported AX success but the text did not appear in the field (common in Electron apps)."
        case .eventCreationFailed:
            return "Could not create synthetic keyboard events."
        case .allStrategiesFailed(let failures, let secure):
            let list = failures.map(\.description).joined(separator: "; ")
            let secureNote = secure ? " Secure Keyboard Entry is ENABLED, which can block synthetic input." : ""
            return "All insertion strategies failed [\(list)].\(secureNote)"
        }
    }
}
