import FluidAudio
import Foundation
import LFContracts
import os

public enum EngineError: Error, CustomStringConvertible {
    case unsupportedSampleRate(got: Double, required: Double)
    case utteranceTooShort(sampleCount: Int)
    case audioLoadFailed(String)

    public var description: String {
        switch self {
        case .unsupportedSampleRate(let got, let required):
            return "Unsupported sample rate \(got) Hz (engine requires \(required) Hz)."
        case .utteranceTooShort(let count):
            return "Utterance too short to transcribe (\(count) samples)."
        case .audioLoadFailed(let why):
            return "Could not load audio: \(why)"
        }
    }
}

/// Download/load progress for the speech model, for onboarding UI.
public struct EngineModelProgress: Sendable {
    public let fractionCompleted: Double
    public let phase: String
}

// Parakeet TDT v3 (0.6B, CoreML via FluidAudio, Apache 2.0): LocalFlow's
// speech engine. Transcribes verbatim (contractions preserved) and emits its
// own punctuation/capitalization — no separate formatter model needed.
// (Granite TurboCTC was removed: its training normalized contractions away
// and its pause-based punctuation formatter split clauses mid-sentence.)
public final class ParakeetTranscriber: Transcriber, @unchecked Sendable {
    private static let logger = Logger(subsystem: "com.localflow.engine", category: "parakeet")

    /// Observed by the app for the onboarding "Models" row.
    public static var progressHandler: (@Sendable (EngineModelProgress) -> Void)? {
        get { progressLock.withLock { _progressHandler } }
        set { progressLock.withLock { _progressHandler = newValue } }
    }
    nonisolated(unsafe) private static var _progressHandler: (@Sendable (EngineModelProgress) -> Void)?
    private static let progressLock = NSLock()

    /// Models live in FluidAudio's own Application Support cache.
    public static var isModelDownloaded: Bool {
        AsrModels.modelsExist(at: AsrModels.defaultCacheDirectory())
    }

    private let lock = NSLock()
    private var manager: AsrManager?
    private var loading = false

    public init() {}

    public func transcribe(_ utterance: Utterance) async throws -> String {
        guard utterance.sampleRate == 16_000 else {
            throw EngineError.unsupportedSampleRate(got: utterance.sampleRate, required: 16_000)
        }
        guard utterance.samples.count > 256 else {
            throw EngineError.utteranceTooShort(sampleCount: utterance.samples.count)
        }
        let manager = try await loadedManager()
        // Fresh decoder state per utterance: dictations are independent.
        var decoderState = try TdtDecoderState()
        let result = try await manager.transcribe(utterance.samples, decoderState: &decoderState)
        return result.text.trimmingCharacters(in: CharacterSet.whitespacesAndNewlines)
    }

    /// Load models ahead of the first utterance (downloads on first ever use).
    public func prepare() async {
        _ = try? await loadedManager()
    }

    private func withState<T>(_ body: (inout AsrManager?, inout Bool) -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body(&manager, &loading)
    }

    private func loadedManager() async throws -> AsrManager {
        if let cached = withState({ manager, loading -> AsrManager? in
            if manager == nil { loading = true }
            return manager
        }) {
            return cached
        }

        do {
            let models = try await AsrModels.downloadAndLoad(
                version: .v3,
                progressHandler: { progress in
                    ParakeetTranscriber.progressHandler?(EngineModelProgress(
                        fractionCompleted: progress.fractionCompleted,
                        phase: String(describing: progress.phase)))
                }
            )
            let loaded = AsrManager(config: .default)
            try await loaded.loadModels(models)
            withState { manager, loading in
                manager = loaded
                loading = false
            }
            Self.progressHandler?(EngineModelProgress(fractionCompleted: 1, phase: "complete"))
            Self.logger.info("Parakeet TDT v3 loaded")
            return loaded
        } catch {
            withState { _, loading in loading = false }
            Self.logger.error("Parakeet load failed: \(String(describing: error), privacy: .public)")
            throw error
        }
    }
}
