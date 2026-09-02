import Foundation

// FROZEN CONTRACTS — streams code against these; only the orchestrator changes them.

/// A finished, endpointed chunk of speech ready for transcription.
public struct Utterance: Sendable {
    /// Mono PCM samples.
    public let samples: [Float]
    /// Expected to be 16_000 for the speech engine.
    public let sampleRate: Double

    public init(samples: [Float], sampleRate: Double) {
        self.samples = samples
        self.sampleRate = sampleRate
    }

    public var duration: TimeInterval {
        Double(samples.count) / sampleRate
    }
}

/// ASR: utterance in, punctuated/cased text out (formatter already applied).
public protocol Transcriber: Sendable {
    func transcribe(_ utterance: Utterance) async throws -> String
}

/// Events from the hold-to-talk capture pipeline.
public enum CaptureEvent: Sendable {
    case began
    /// Hotkey released; utterance is ready for transcription.
    case ended(Utterance)
    /// User cancelled (e.g. Esc) or the recording was too short.
    case cancelled
    /// Input level 0...1 for HUD metering.
    case level(Float)
}

/// Hold-to-talk hotkey + microphone capture.
public protocol CaptureEngine: Sendable {
    /// Install event tap + prepare audio. Throws if permissions are missing.
    func start() throws
    func stop()
    var events: AsyncStream<CaptureEvent> { get }
}

public struct PolishContext: Sendable {
    /// Bundle id of the app that will receive the text, when known.
    public let targetAppBundleID: String?

    public init(targetAppBundleID: String? = nil) {
        self.targetAppBundleID = targetAppBundleID
    }
}

/// Cleanup pass. MUST fail open: on error, timeout, or unavailability,
/// return the input (with deterministic replacements still applied if possible).
public protocol TextPolisher: Sendable {
    func polish(_ text: String, context: PolishContext) async -> String
}

/// Inserts text at the caret of the frontmost app.
public protocol TextInserter: Sendable {
    func insert(_ text: String) async throws
}
