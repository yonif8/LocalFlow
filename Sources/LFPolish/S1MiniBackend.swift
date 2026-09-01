import Foundation
import HuggingFace
import MLX
import MLXHuggingFace
import MLXLLM
import MLXLMCommon
import Tokenizers
import os

// S1-mini by Superwhisper (Apache 2.0 with attribution clause): a 0.6B
// Qwen3 fine-tune purpose-built for ASR transcript cleanup — filler removal,
// false-start/self-correction resolution, punctuation. It is a text
// normalizer, not a chat model: it takes a control line + transcript and
// cannot drift into answering the dictation.
//
// Measured on this machine (M1 Max, 8-bit): ~0.9s load, ~0.1s per utterance.
final class S1MiniBackend: PolishModel, @unchecked Sendable {
    /// One model in memory per process, shared by every LocalPolisher.
    static let shared = S1MiniBackend()

    static let modelID = "mlx-community/S1-mini-MLX-8bit"

    // Verbatim required system prompt from the S1-mini model card.
    private static let systemPrompt = """
        You are a text normalizer for speech-to-text transcripts. The input begins \
        with a control line specifying the styling, structure, and context settings; \
        clean the transcript to match those settings and output only the cleaned text.
        """

    private static let logger = Logger(subsystem: "com.localflow.polish", category: "s1mini")

    private let lock = NSLock()
    private var container: ModelContainer?
    private var loadFailure: String?
    private var loading = false

    var availability: ModelAvailability {
        lock.lock()
        defer { lock.unlock() }
        if container != nil { return .available }
        if let loadFailure { return .unavailable("S1-mini load failed: \(loadFailure)") }
        if loading { return .unavailable("S1-mini still loading/downloading") }
        // Not loaded yet — prewarm will load it; report as available so the
        // polish path calls respond(), which triggers the load.
        return .available
    }

    func prewarm() {
        Task.detached(priority: .utility) { [self] in
            _ = try? await loadedContainer()
        }
    }

    private func withState<T>(_ body: (inout ModelContainer?, inout String?, inout Bool) -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body(&container, &loadFailure, &loading)
    }

    private func loadedContainer() async throws -> ModelContainer {
        if let cached = withState({ container, _, loading -> ModelContainer? in
            if container == nil { loading = true }
            return container
        }) {
            return cached
        }

        do {
            // Explicit HubClient so the weights live in LocalFlow's own
            // Application Support directory (PolishModelStore.cacheDirectory)
            // instead of ~/.cache/huggingface/hub, with download progress
            // surfaced through PolishModelStore.progressHandler.
            let hub = HubClient(cache: HubCache(cacheDirectory: PolishModelStore.cacheDirectory))
            let loaded = try await loadModelContainer(
                from: #hubDownloader(hub),
                using: #huggingFaceTokenizerLoader(),
                configuration: ModelConfiguration(id: Self.modelID),
                progressHandler: { progress in
                    PolishModelStore.report(progress)
                })
            // Warm generation so the first real polish skips kernel compilation.
            _ = try? await makeSession(loaded).respond(
                to: Self.userMessage("warm up test", tone: .neutral))
            withState { container, _, loading in
                container = loaded
                loading = false
            }
            Self.logger.info("S1-mini loaded and warm")
            return loaded
        } catch {
            withState { _, loadFailure, loading in
                loadFailure = String(describing: error)
                loading = false
            }
            Self.logger.error("S1-mini load failed: \(String(describing: error), privacy: .public)")
            throw error
        }
    }

    private func makeSession(_ container: ModelContainer) -> ChatSession {
        ChatSession(
            container,
            instructions: Self.systemPrompt,
            generateParameters: GenerateParameters(temperature: 0),
            additionalContext: ["enable_thinking": false])
    }

    private static func userMessage(_ input: String, tone: ToneHint) -> String {
        let styling: String
        switch tone {
        case .casual: styling = "semi-casual"
        case .neutral: styling = "semi-formal"
        }
        return "[Styling: \(styling)] [Structure: prose] [Context: general]\n\(input)"
    }

    func respond(input: String, tone: ToneHint) async throws -> String {
        let container = try await loadedContainer()
        // Fresh session per utterance: no context bleed between dictations.
        let output = try await makeSession(container).respond(
            to: Self.userMessage(input, tone: tone))
        // Defensive: strip a thinking block if the template ever lets one through.
        if let range = output.range(of: "</think>") {
            return String(output[range.upperBound...])
        }
        return output
    }
}
