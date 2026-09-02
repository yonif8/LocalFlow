import Foundation
import AppKit
import LFContracts

// Mock pipeline components, coded strictly against the FROZEN LFContracts
// protocols. Status of the real modules as of this stream's work:
//   - LFEngine (Transcriber): intentionally NOT a dependency of this target;
//     orchestrator wires the real one at integration. MockTranscriber stands in.
//   - LFCapture / LFInsert / LFPolish: targets exist as dependencies but their
//     sources are placeholders with no public API yet, so contracts-shaped
//     mocks stand in here too. Swap points are all in DictationCoordinator.init.

/// Warm-up seam mirroring LFEngine's real `ParakeetTranscriber.prepare()`:
/// first-ever inference is ~2.1 s cold vs ~110 ms warm, so the coordinator
/// calls prepare at startup. The orchestrator adapts the real transcriber to
/// this (or calls prepare directly) at integration.
protocol PreparableTranscriber: Sendable {
    func prepare() async
}

/// Returns canned text after a short delay, cycling through phrases.
actor MockTranscriber: Transcriber, PreparableTranscriber {
    /// No-op warm-up (the real engine loads MLX weights here).
    func prepare() async {}
    private var index = 0
    private let phrases = [
        "Hello from LocalFlow — this transcript came from the mock transcriber.",
        "The quick brown fox jumps over the lazy dog.",
        "Remember to pick up oat milk and coffee beans on the way home.",
        "Dictation is working end to end with the mock transcriber.",
        "This sentence was produced entirely offline, no cloud required.",
    ]

    func transcribe(_ utterance: Utterance) async throws -> String {
        // Simulate model latency proportional-ish to utterance length.
        let delay = min(1.5, max(0.6, utterance.duration * 0.3))
        try await Task.sleep(for: .seconds(delay))
        let phrase = phrases[index % phrases.count]
        index += 1
        return phrase
    }
}

/// Mock hold-to-talk capture. `simulateDictation()` drives a full session:
/// .began → ~1.6 s of .level events → .ended(Utterance).
final class MockCaptureEngine: CaptureEngine, @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: AsyncStream<CaptureEvent>.Continuation?
    private var stream: AsyncStream<CaptureEvent>?
    private var running = false
    private var simulationTask: Task<Void, Never>?

    var events: AsyncStream<CaptureEvent> {
        lock.lock()
        defer { lock.unlock() }
        if let stream { return stream }
        let (s, c) = AsyncStream.makeStream(of: CaptureEvent.self)
        stream = s
        continuation = c
        return s
    }

    func start() throws {
        lock.lock()
        defer { lock.unlock() }
        running = true
    }

    func stop() {
        lock.lock()
        simulationTask?.cancel()
        simulationTask = nil
        running = false
        continuation?.finish()
        continuation = nil
        stream = nil
        lock.unlock()
    }

    /// Debug hook (not part of the CaptureEngine contract).
    func simulateDictation(utterance override: Utterance? = nil) {
        lock.lock()
        guard running, simulationTask == nil, let continuation else {
            lock.unlock()
            return
        }
        lock.unlock()

        simulationTask = Task { [weak self] in
            continuation.yield(.began)
            // ~1.6 s of levels at 20 Hz: a speech-ish envelope.
            let steps = 32
            for i in 0..<steps {
                guard !Task.isCancelled else { return }
                try? await Task.sleep(for: .milliseconds(50))
                let t = Float(i) / Float(steps)
                let envelope = sinf(t * .pi)                     // rise and fall
                let wobble = 0.25 * sinf(t * 43) + 0.15 * sinf(t * 17)
                continuation.yield(.level(max(0.05, min(1, envelope * 0.8 + wobble * envelope))))
            }
            if let override {
                // Real spoken audio injected (LOCALFLOW_SIM_WAV) so the real
                // engine produces a real transcript during simulation.
                continuation.yield(.ended(override))
            } else {
                // 1.6 s of 440 Hz sine at 16 kHz so the Utterance is shaped like real audio.
                let sampleRate = 16_000.0
                let count = Int(sampleRate * 1.6)
                let samples = (0..<count).map { i in
                    0.1 * sinf(2 * .pi * 440 * Float(i) / Float(sampleRate))
                }
                continuation.yield(.ended(Utterance(samples: samples, sampleRate: sampleRate)))
            }
            self?.clearSimulationTask()
        }
    }

    private func clearSimulationTask() {
        lock.lock()
        simulationTask = nil
        lock.unlock()
    }
}

/// Fail-open polisher: trims whitespace, otherwise returns input unchanged.
/// Stands in for LFPolish until it exposes a public TextPolisher.
struct PassthroughPolisher: TextPolisher {
    func polish(_ text: String, context: PolishContext) async -> String {
        text.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

/// Copies the text to the general pasteboard so the demo produces a tangible
/// result without Accessibility permission. Stands in for LFInsert until it
/// exposes a public TextInserter (real one types at the caret).
struct PasteboardInserter: TextInserter {
    func insert(_ text: String) async throws {
        await MainActor.run {
            let pasteboard = NSPasteboard.general
            pasteboard.clearContents()
            pasteboard.setString(text, forType: .string)
        }
    }
}
