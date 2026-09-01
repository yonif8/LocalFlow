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
    /// Load the model ahead of the first respond call. Default: no-op.
    func prewarm()
}

extension PolishModel {
    func prewarm() {}
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
        /// The model's output did not look like a cleanup of the input
        /// (e.g. it answered the dictation instead of cleaning it).
        case implausibleOutput
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
        /// Inputs longer than this skip the LLM pass instantly (never
        /// truncates). The bound is LATENCY, not context: the on-device model
        /// regenerates the whole text and manages roughly two sentences
        /// inside the 2s budget on an M1 Max — beyond that the pass would
        /// only burn the full timeout and fail open anyway.
        public var maxInputCharacters: Int

        public init(
            llmEnabled: Bool = true,
            timeout: TimeInterval = 1.5,
            maxInputCharacters: Int = 700
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
        // S1-mini (purpose-built transcript normalizer, ~0.1s/utterance on
        // M1 Max) replaced the Apple Foundation Models backend, which was
        // ~10-20x slower and prone to answering the dictation. The FM
        // backend remains in-tree but unwired.
        S1MiniBackend.shared
    }

    /// Load the on-device model ahead of the first polish. A cold
    /// LanguageModelSession pays multi-second model-load latency on its first
    /// respond — enough to blow the polish timeout on every utterance — and
    /// the timed-out request keeps computing, starving the ASR engine's GPU
    /// work. Model loading is process-wide, so one prewarm makes every
    /// subsequent per-call session fast.
    public func prewarm() {
        model?.prewarm()
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
            Self.logger.info("""
                polish fail-open: \(String(describing: reason), privacy: .public) \
                (llm \(llmDuration.map { String(describing: $0) } ?? "-", privacy: .public))
                """)
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
        let availabilityStart = clock.now
        let availability = model.availability
        let availabilityDuration = clock.now - availabilityStart
        Self.logger.info("availability check took \(String(describing: availabilityDuration), privacy: .public)")
        if case .unavailable(let reason) = availability {
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
            guard Self.looksLikeCleanup(of: replaced, candidate: polishedText) else {
                Self.logger.warning("model output rejected as implausible: \(polishedText, privacy: .public)")
                return failOpen(.implausibleOutput, llmDuration: llmDuration)
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

    // MARK: Output plausibility

    /// A cleanup's output must be recognizably the input's own words. The
    /// model sometimes ANSWERS the dictation ("can you send the report?" →
    /// "Sure, I can send it") or emits canned assistant text; pasting that
    /// as the user's words is far worse than skipping the polish.
    static func looksLikeCleanup(of input: String, candidate: String) -> Bool {
        func contentWords(_ s: String) -> Set<String> {
            Set(
                s.lowercased()
                    .split(whereSeparator: { !$0.isLetter && !$0.isNumber })
                    .map(String.init)
                    .filter { $0.count > 2 }
            )
        }
        // Length must be in the ballpark: filler removal shrinks a little,
        // cleanup never triples or guts the text.
        guard candidate.count * 3 >= input.count, candidate.count <= input.count * 2 else {
            return false
        }
        let inputWords = contentWords(input)
        let candidateWords = contentWords(candidate)
        // Degenerate inputs (all short words) can't be scored; let them pass
        // the length check alone.
        guard !inputWords.isEmpty, !candidateWords.isEmpty else { return true }
        let overlap = candidateWords.intersection(inputWords).count
        return Double(overlap) >= 0.6 * Double(candidateWords.count)
    }

    // MARK: Timeout

    struct TimeoutError: Error {}

    /// Races `operation` against a wall-clock deadline and RETURNS AT THE
    /// DEADLINE regardless. A task group is deliberately not used: a group
    /// waits for all children before returning, so an operation that ignores
    /// cancellation (the Foundation Models call does) would hold the
    /// "timeout" hostage until it finished anyway — observed as 8-10s
    /// fail-opens against a 2s budget. The losing task is cancelled
    /// best-effort and abandoned; its late result is discarded.
    /// One-shot claim guard for racing continuation resumers.
    private final class ResumeOnce: @unchecked Sendable {
        private let lock = NSLock()
        private var resumed = false
        func claim() -> Bool {
            lock.lock(); defer { lock.unlock() }
            if resumed { return false }
            resumed = true
            return true
        }
    }

    static func withTimeout<T: Sendable>(
        _ seconds: TimeInterval,
        operation: @escaping @Sendable () async throws -> T
    ) async throws -> T {
        let once = ResumeOnce()
        return try await withCheckedThrowingContinuation { continuation in
            let work = Task {
                do {
                    let value = try await operation()
                    if once.claim() { continuation.resume(returning: value) }
                } catch {
                    if once.claim() { continuation.resume(throwing: error) }
                }
            }
            Task {
                try? await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
                if once.claim() {
                    work.cancel()
                    continuation.resume(throwing: TimeoutError())
                }
            }
        }
    }
}
