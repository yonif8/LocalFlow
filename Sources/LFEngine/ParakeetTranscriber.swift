import FluidAudio
import Foundation
import LFContracts
import os

// Parakeet TDT v3 (0.6B, CoreML via FluidAudio, Apache 2.0): the alternate
// ASR engine. Unlike Granite TurboCTC — whose training data normalizes
// contractions away ("don't" can only ever come out "do not") — Parakeet
// transcribes verbatim AND emits its own punctuation/capitalization, so the
// pause-based formatter isn't needed on this path.
public final class ParakeetTranscriber: Transcriber, @unchecked Sendable {
    private static let logger = Logger(subsystem: "com.localflow.engine", category: "parakeet")

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
            let models = try await AsrModels.downloadAndLoad(version: .v3)
            let loaded = AsrManager(config: .default)
            try await loaded.loadModels(models)
            withState { manager, loading in
                manager = loaded
                loading = false
            }
            Self.logger.info("Parakeet TDT v3 loaded")
            return loaded
        } catch {
            withState { _, loading in loading = false }
            Self.logger.error("Parakeet load failed: \(String(describing: error), privacy: .public)")
            throw error
        }
    }
}
