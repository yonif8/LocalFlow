import AppKit
import LFContracts
import os

/// Inserts text at the caret of the frontmost app via a configurable strategy
/// chain (AX selected-text first, transient-pasteboard paste as fallback by
/// default). Never activates this process or steals focus from the target app.
public final class FrontmostInserter: TextInserter, Sendable {
    public let configuration: InserterConfiguration

    public init(configuration: InserterConfiguration = .default) {
        self.configuration = configuration
    }

    // MARK: TextInserter

    public func insert(_ text: String) async throws {
        _ = try await insertReporting(text)
    }

    /// Like `insert(_:)` but reports which strategy succeeded (for logging/HUD).
    @discardableResult
    public func insertReporting(_ text: String) async throws -> InsertionOutcome {
        guard !text.isEmpty else { throw InsertionError.emptyText }
        guard InsertPermissions.accessibilityGranted() else {
            throw InsertionError.accessibilityNotGranted
        }
        let secureInput = InsertPermissions.secureInputEnabled

        let target = NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? "unknown"
        var failures: [StrategyFailure] = []
        for strategy in configuration.strategyOrder {
            do {
                try await attempt(strategy, text: text)
                Logger(subsystem: "com.localflow.insert", category: "insert").info("""
                    inserted via \(String(describing: strategy), privacy: .public) \
                    into \(target, privacy: .public)\
                    \(failures.isEmpty ? "" : " (after: \(failures.map(String.init(describing:)).joined(separator: "; ")))", privacy: .public)
                    """)
                return InsertionOutcome(
                    strategy: strategy,
                    secureInputWasEnabled: secureInput,
                    earlierFailures: failures
                )
            } catch {
                let reason = (error as? InsertionError).map(String.init(describing:))
                    ?? error.localizedDescription
                failures.append(StrategyFailure(strategy: strategy, reason: reason))
            }
        }
        throw InsertionError.allStrategiesFailed(
            failures: failures, secureInputWasEnabled: secureInput
        )
    }

    // MARK: - Strategies

    private func attempt(_ strategy: InsertionStrategy, text: String) async throws {
        switch strategy {
        case .ax:
            try await MainActor.run {
                try AXInsertion.insert(text, verify: configuration.verifyAXInsertion)
            }
        case .paste:
            try await pasteInsert(text)
        case .type:
            try KeyEvents.typeUnicode(
                text,
                chunkSize: configuration.typingChunkSize,
                interChunkDelay: configuration.typingInterChunkDelay
            )
        }
    }

    /// Save pasteboard → write transient string → Cmd-V → restore after a delay.
    /// The restore is skipped if something else wrote to the pasteboard during
    /// the delay (so we never clobber a user copy that raced us).
    @MainActor
    private func pasteInsert(_ text: String) async throws {
        let pasteboard = NSPasteboard.general
        let saved = PasteboardTransfer.snapshot(of: pasteboard)
        let ourChangeCount = PasteboardTransfer.setTransientString(text, on: pasteboard)

        do {
            try KeyEvents.postCommandV()
        } catch {
            // Paste never happened; put the old contents back immediately.
            PasteboardTransfer.restore(
                saved, to: pasteboard, ifChangeCountStillEquals: ourChangeCount
            )
            throw error
        }

        // Give the target app time to service the paste before yanking the
        // contents back (slow pasters — Electron — read asynchronously).
        let delay = configuration.pasteboardRestoreDelay
        if delay > 0 {
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
        }
        PasteboardTransfer.restore(
            saved, to: pasteboard, ifChangeCountStillEquals: ourChangeCount
        )
    }
}
