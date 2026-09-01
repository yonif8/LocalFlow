import Foundation
import GraniteMLX
import LFContracts

/// Errors produced by `GraniteTranscriber` before handing off to GraniteMLX.
public enum EngineError: Error, CustomStringConvertible {
    /// The utterance sample rate does not match the model's required rate (16 kHz).
    case unsupportedSampleRate(got: Double, required: Double)
    /// The utterance is too short for the model (minimum 257 samples ≈ 16 ms).
    case utteranceTooShort(sampleCount: Int)

    public var description: String {
        switch self {
        case .unsupportedSampleRate(let got, let required):
            return "Unsupported sample rate \(got) Hz; the Granite pipeline requires \(required) Hz mono."
        case .utteranceTooShort(let sampleCount):
            return "Utterance too short (\(sampleCount) samples); at least 257 samples (~16 ms at 16 kHz) are required."
        }
    }
}

/// Model download/cache progress surfaced to the app (e.g. for a first-run HUD).
public struct EngineModelProgress: Sendable {
    /// Hugging Face repository ID being acquired (speech or punctuation model).
    public let repositoryID: String
    /// Coarse phase description ("checking", "downloading", "cache_hit", "complete").
    public let phase: String
    /// 0...1 byte-weighted progress for the current model.
    public let fractionCompleted: Double
    /// Approximate final size in bytes, when known (for "X of Y MB" UI).
    public let estimatedTotalBytes: Int64?
}

/// Canonical on-disk locations for LocalFlow's speech + punctuation models.
///
/// Everything lives under `~/Library/Application Support/LocalFlow/granite/`
/// (materialized checkpoints at `granite/models/<owner>/<repo>`, Hugging Face
/// transfer cache at `hf-cache/`). The `LOCALFLOW_MODELS_ROOT` environment
/// variable overrides the root — a debug/testing hook, never needed in
/// normal operation.
public enum EngineModelLocations {
    /// Root directory holding every LocalFlow model subdirectory.
    public static var rootDirectory: URL {
        if let override = ProcessInfo.processInfo.environment["LOCALFLOW_MODELS_ROOT"],
           !override.isEmpty {
            return URL(
                fileURLWithPath: NSString(string: override).expandingTildeInPath,
                isDirectory: true)
        }
        let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return appSupport.appendingPathComponent("LocalFlow", isDirectory: true)
    }

    /// Storage configuration used by every Granite model operation.
    public static var graniteStorage: GraniteModelStorage {
        GraniteModelStorage(
            hubDirectory: rootDirectory.appendingPathComponent("granite", isDirectory: true),
            downloadCacheDirectory: rootDirectory.appendingPathComponent(
                "hf-cache", isDirectory: true),
            compiledCoreMLDirectory: nil)
    }

    /// Directory containing materialized `owner/repo` model folders.
    public static var modelsDirectory: URL {
        graniteStorage.modelsDirectory
    }

    /// True when a complete speech checkpoint is present locally.
    public static func isSpeechModelDownloaded() -> Bool {
        GraniteModelManager(storage: graniteStorage)
            .isDownloaded(GraniteModelLoader.defaultModelID)
    }

    /// True when a complete punctuation/truecase checkpoint is present locally.
    public static func isPunctuationModelDownloaded() -> Bool {
        GraniteModelManager(storage: graniteStorage)
            .isDownloaded(PunctuationModelLoader.defaultModelID)
    }
}

/// Per-call timing breakdown, for the CLI and latency instrumentation.
public struct TranscriptionTimings: Sendable {
    /// Time spent loading models, or nil when they were already resident.
    public let modelLoad: TimeInterval?
    /// CTC recognizer inference time (includes feature extraction).
    public let inference: TimeInterval
    /// Punctuation/truecase formatter time.
    public let formatting: TimeInterval

    public var total: TimeInterval { (modelLoad ?? 0) + inference + formatting }
}

/// `Transcriber` backed by Granite-MLX (MLX backend) plus the author's
/// punctuation/truecase formatter.
///
/// Models are loaded lazily on first use (downloading into the shared
/// Granite-MLX cache if absent) and kept resident for the lifetime of the
/// instance. All GraniteMLX calls are synchronous and are executed on a
/// private serial queue, never on the caller's thread or the main actor.
///
/// Formatting fails open: if the punctuation formatter throws, the raw
/// (lowercase, unpunctuated) transcript is returned rather than failing the
/// whole dictation.
public final class GraniteTranscriber: Transcriber, @unchecked Sendable {

    public struct Configuration: Sendable {
        /// Speech model: catalog alias, local directory, or HF repository ID.
        public var speechModel: String
        /// Punctuation/truecase formatter model.
        public var formatterModel: String
        /// When false, raw lowercase CTC text is returned (no formatter load).
        public var punctuate: Bool
        /// Where models are materialized/downloaded. Defaults to
        /// `EngineModelLocations.graniteStorage` (LocalFlow's Application
        /// Support directory).
        public var storage: GraniteModelStorage
        /// Called with model download/cache progress during first load.
        public var progressHandler: (@Sendable (EngineModelProgress) -> Void)?

        public init(
            speechModel: String = GraniteModelLoader.defaultModelID,
            formatterModel: String = PunctuationModelLoader.defaultModelID,
            punctuate: Bool = true,
            storage: GraniteModelStorage = EngineModelLocations.graniteStorage,
            progressHandler: (@Sendable (EngineModelProgress) -> Void)? = nil
        ) {
            self.speechModel = speechModel
            self.formatterModel = formatterModel
            self.punctuate = punctuate
            self.storage = storage
            self.progressHandler = progressHandler
        }
    }

    private let configuration: Configuration
    /// Serial queue owning all GraniteMLX state and calls. `recognizer` and
    /// `formatter` are only touched from this queue, which is what makes the
    /// `@unchecked Sendable` conformance sound.
    private let queue = DispatchQueue(label: "com.localflow.engine.granite", qos: .userInitiated)
    private var recognizer: GraniteRecognizer?
    private var formatter: (any GraniteTranscriptFormatter)?

    public init(configuration: Configuration = Configuration()) {
        self.configuration = configuration
    }

    // MARK: - Transcriber

    public func transcribe(_ utterance: Utterance) async throws -> String {
        try await transcribeWithTimings(utterance).text
    }

    // MARK: - Instrumented API

    /// Transcribes and reports where the time went. `modelLoad` is non-nil only
    /// for the call that actually loaded (or downloaded) the models.
    public func transcribeWithTimings(
        _ utterance: Utterance
    ) async throws -> (text: String, timings: TranscriptionTimings) {
        let detailed = try await transcribeDetailed(utterance)
        return (detailed.formatted, detailed.timings)
    }

    /// Like `transcribeWithTimings`, but also returns the RAW CTC text
    /// (lowercase, unpunctuated). A downstream normalizer that punctuates
    /// (S1-mini) does markedly better from raw text than from the small
    /// formatter model's output — the formatter punctuates on acoustic
    /// pauses and splits clauses mid-sentence; feeding its guesses to the
    /// normalizer locks them in. `formatted` remains the fail-open fallback.
    public func transcribeDetailed(
        _ utterance: Utterance
    ) async throws -> (raw: String, formatted: String, timings: TranscriptionTimings) {
        guard utterance.sampleRate == 16_000 else {
            throw EngineError.unsupportedSampleRate(got: utterance.sampleRate, required: 16_000)
        }
        guard utterance.samples.count > 256 else {
            throw EngineError.utteranceTooShort(sampleCount: utterance.samples.count)
        }
        let samples = utterance.samples
        return try await onQueue { [self] in
            let loadStart = ContinuousClock.now
            let didLoad = try loadModelsIfNeededLocked()
            let loadDuration = didLoad ? seconds(since: loadStart) : nil

            let audio = GraniteAudio(
                samples: samples,
                sampleRate: 16_000,
                source: URL(fileURLWithPath: "/in-memory/utterance")
            )
            let inferenceStart = ContinuousClock.now
            let transcription = try recognizer!.transcribe(audio)
            let inferenceDuration = seconds(since: inferenceStart)

            let raw = transcription.text
            var text = raw
            var formattingDuration: TimeInterval = 0
            if let formatter {
                let formattingStart = ContinuousClock.now
                do {
                    let formatted = try formatter.format(
                        raw, cancellationToken: nil, progressHandler: nil)
                    text = formatted.text
                } catch {
                    // Fail open: raw transcript is better than no transcript.
                    FileHandle.standardError.write(
                        Data("[LFEngine] formatter failed, returning raw text: \(error)\n".utf8))
                }
                formattingDuration = seconds(since: formattingStart)
            }
            let timings = TranscriptionTimings(
                modelLoad: loadDuration,
                inference: inferenceDuration,
                formatting: formattingDuration
            )
            return (
                raw.trimmingCharacters(in: .whitespacesAndNewlines),
                text.trimmingCharacters(in: .whitespacesAndNewlines),
                timings
            )
        }
    }

    /// Loads (downloading if needed) the models ahead of the first utterance,
    /// and optionally runs a short warm-up inference so the first real
    /// dictation does not pay Metal kernel warm-up cost.
    /// - Returns: Time spent loading, or nil if the models were already resident.
    @discardableResult
    public func prepare(warmRun: Bool = true) async throws -> TimeInterval? {
        try await onQueue { [self] in
            let start = ContinuousClock.now
            let didLoad = try loadModelsIfNeededLocked()
            let loadDuration = didLoad ? seconds(since: start) : nil
            if warmRun {
                // 0.5 s of near-silence; output is discarded.
                let warm = GraniteAudio(
                    samples: [Float](repeating: 0.0001, count: 8_000),
                    sampleRate: 16_000,
                    source: URL(fileURLWithPath: "/in-memory/warmup")
                )
                _ = try? recognizer!.transcribe(warm)
                if let formatter {
                    _ = try? formatter.format(
                        "warm up", cancellationToken: nil, progressHandler: nil)
                }
            }
            return loadDuration
        }
    }

    // MARK: - Internals

    /// Must be called on `queue`. Returns true when this call performed a load.
    private func loadModelsIfNeededLocked() throws -> Bool {
        guard recognizer == nil else { return false }
        var downloadHandler: GraniteModelDownloadProgressHandler?
        if let handler = configuration.progressHandler {
            downloadHandler = { (update: GraniteModelDownloadProgress) in
                handler(EngineModelProgress(
                    repositoryID: update.repositoryID,
                    phase: update.phase.rawValue,
                    fractionCompleted: update.fractionCompleted,
                    estimatedTotalBytes: update.estimatedTotalBytes
                ))
            }
        }
        recognizer = try GraniteRecognizer(
            modelSource: configuration.speechModel,
            storage: configuration.storage,
            progressHandler: downloadHandler
        )
        if configuration.punctuate {
            formatter = try GraniteTranscriptFormatterFactory.load(
                modelSource: configuration.formatterModel,
                storage: configuration.storage,
                progressHandler: downloadHandler
            )
        }
        return true
    }

    private func onQueue<T: Sendable>(
        _ body: @escaping @Sendable () throws -> T
    ) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                continuation.resume(with: Result { try body() })
            }
        }
    }

    private func seconds(since start: ContinuousClock.Instant) -> TimeInterval {
        let duration = start.duration(to: .now)
        return Double(duration.components.seconds)
            + Double(duration.components.attoseconds) / 1e18
    }
}
