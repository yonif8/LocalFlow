import Foundation
import LFContracts
import os

// MARK: - Stage 2 abstraction (internal so tests can inject mocks).

/// Tone hint derived from the target app's bundle id.
enum ToneHint: Sendable, Equatable {
    case casual
    case neutral

    /// The one small table mapping bundle ids to a tone.
    static let casualBundleIDs: Set<String> = [
        "com.tinyspeck.slackmacgap",   // Slack
        "com.hnc.Discord",             // Discord
        "com.apple.MobileSMS",         // Messages
        "com.apple.iChat",             // Messages (legacy id)
        "net.whatsapp.WhatsApp",       // WhatsApp
        "ru.keepcoder.Telegram",       // Telegram (Mac App Store)
        "org.telegram.desktop",        // Telegram Desktop
    ]

    init(bundleID: String?) {
        if let bundleID, Self.casualBundleIDs.contains(bundleID) {
            self = .casual
        } else {
            self = .neutral
        }
    }
}

enum ModelAvailability: Sendable, Equatable {
    case available
    case unavailable(String)

    var description: String {
        switch self {
        case .available: return "available"
        case .unavailable(let reason): return "unavailable (\(reason))"
        }
    }
}

/// Internal seam for the LLM pass so tests can simulate slow/failing models.
protocol PolishModel: Sendable {
    var availability: ModelAvailability { get }
    func respond(input: String, tone: ToneHint) async throws -> String
}

// MARK: - Outcome reporting (for the app's HUD / debugging).

/// Which path `polish` took. The text result is always usable either way.
public enum PolishOutcome: Sendable, Equatable {
    /// Stage 1 + LLM polish both applied.
    case polished
    /// Only deterministic replacements were applied (fail-open or LLM off).
    case replacementsOnly(SkipReason)

    public enum SkipReason: Sendable, Equatable {
        case llmDisabled
        case modelUnavailable(String)
        case inputTooLong
        case timeout
        case error(String)
        case emptyModelOutput
    }
}

/// Full result of a polish run, for the HUD and for polish-cli.
public struct PolishResult: Sendable {
    /// Final text (LLM-polished when possible, else the stage-1 text).
    public let text: String
    /// Output of stage 1 (deterministic replacements) alone.
    public let afterReplacements: String
    public let outcome: PolishOutcome
    /// Human-readable model availability, e.g. for a status line.
    public let modelAvailability: String
    public let replacementsDuration: Duration
    public let llmDuration: Duration?
}

// MARK: - LocalPolisher

/// The LFPolish `TextPolisher`: deterministic replacements always run;
/// the on-device LLM pass is best-effort and strictly fail-open.
public struct LocalPolisher: TextPolisher {
    public struct Configuration: Sendable {
        /// Master switch for the LLM pass.
        public var llmEnabled: Bool
        /// Wall-clock budget for the LLM pass; on expiry, fail open.
        public var timeout: TimeInterval
        /// Inputs longer than this skip the LLM pass entirely (the model's
        /// context budget is 4096 tokens total; ~6000 chars keeps instructions
        /// + input + output comfortably inside it). Never truncates.
        public var maxInputCharacters: Int

        public init(
            llmEnabled: Bool = true,
            timeout: TimeInterval = 2.0,
            maxInputCharacters: Int = 6000
        ) {
            self.llmEnabled = llmEnabled
            self.timeout = timeout
            self.maxInputCharacters = maxInputCharacters
        }
    }

    private let engine: ReplacementEngine
    private let configuration: Configuration
    private let model: PolishModel?
    private static let logger = Logger(subsystem: "com.localflow.polish", category: "polish")

    public init(dictionary: PersonalDictionary = PersonalDictionary(),
                configuration: Configuration = Configuration()) {
        self.init(
            dictionary: dictionary,
            configuration: configuration,
            model: Self.makeDefaultModel())
    }

    /// Internal injection point for tests.
    init(dictionary: PersonalDictionary, configuration: Configuration, model: PolishModel?) {
        self.engine = ReplacementEngine(dictionary: dictionary)
        self.configuration = configuration
        self.model = model
    }

    static func makeDefaultModel() -> PolishModel? {
        #if canImport(FoundationModels)
        if #available(macOS 26.0, *) {
            return FoundationModelBackend()
        }
        #endif
        return nil
    }

    /// Human-readable availability of the on-device model, for status UIs.
    public var modelAvailabilityDescription: String {
        guard let model else { return "unavailable (FoundationModels not present in this build/OS)" }
        return model.availability.description
    }

    // MARK: TextPolisher

    public func polish(_ text: String, context: PolishContext) async -> String {
        await polishDetailed(text, context: context).text
    }

    /// Like `polish`, but reports which path was taken. Never throws.
    public func polishDetailed(_ text: String, context: PolishContext) async -> PolishResult {
        let clock = ContinuousClock()

        let replacementsStart = clock.now
        let replaced = engine.apply(to: text)
        let replacementsDuration = clock.now - replacementsStart

        func failOpen(_ reason: PolishOutcome.SkipReason, llmDuration: Duration? = nil) -> PolishResult {
            Self.logger.info("polish fail-open: \(String(describing: reason), privacy: .public)")
            return PolishResult(
                text: replaced,
                afterReplacements: replaced,
                outcome: .replacementsOnly(reason),
                modelAvailability: modelAvailabilityDescription,
                replacementsDuration: replacementsDuration,
                llmDuration: llmDuration)
        }

        guard configuration.llmEnabled else { return failOpen(.llmDisabled) }
        guard let model else {
            return failOpen(.modelUnavailable("FoundationModels not present in this build/OS"))
        }
        if case .unavailable(let reason) = model.availability {
            return failOpen(.modelUnavailable(reason))
        }
        guard replaced.count <= configuration.maxInputCharacters else {
            return failOpen(.inputTooLong)
        }
        let trimmedInput = replaced.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedInput.isEmpty else { return failOpen(.emptyModelOutput) }

        let tone = ToneHint(bundleID: context.targetAppBundleID)
        let start = clock.now
        do {
            let output = try await Self.withTimeout(configuration.timeout) {
                try await model.respond(input: replaced, tone: tone)
            }
            let llmDuration = clock.now - start
            let polishedText = output.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !polishedText.isEmpty else {
                return failOpen(.emptyModelOutput, llmDuration: llmDuration)
            }
            Self.logger.info("polish succeeded via on-device model")
            return PolishResult(
                text: polishedText,
                afterReplacements: replaced,
                outcome: .polished,
                modelAvailability: modelAvailabilityDescription,
                replacementsDuration: replacementsDuration,
                llmDuration: llmDuration)
        } catch is TimeoutError {
            return failOpen(.timeout, llmDuration: clock.now - start)
        } catch {
            return failOpen(.error(String(describing: error)), llmDuration: clock.now - start)
        }
    }

    // MARK: Timeout

    struct TimeoutError: Error {}

    /// Races `operation` against a wall-clock deadline; the loser is cancelled.
    static func withTimeout<T: Sendable>(
        _ seconds: TimeInterval,
        operation: @escaping @Sendable () async throws -> T
    ) async throws -> T {
        try await withThrowingTaskGroup(of: T.self) { group in
            group.addTask { try await operation() }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
                throw TimeoutError()
            }
            do {
                guard let first = try await group.next() else { throw TimeoutError() }
                group.cancelAll()
                return first
            } catch {
                group.cancelAll()
                throw error
            }
        }
    }
}
