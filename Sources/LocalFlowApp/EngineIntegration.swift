import Foundation
import LFContracts
import LFEngine

// Orchestrator integration: the real Granite transcriber, adapted to the
// app's warm-up seam. Weights load lazily inside GraniteTranscriber, so
// construction is cheap; prepare() pulls the ~2.1 s cold start forward to
// app startup.
extension GraniteTranscriber: PreparableTranscriber {
    func prepare() async {
        _ = try? await prepare(warmRun: true)
    }
}

enum EngineFactory {
    static func makeTranscriber() -> GraniteTranscriber {
        GraniteTranscriber()
    }

    /// Debug: LOCALFLOW_SIM_WAV=<path to 16 kHz wav> makes "Simulate
    /// Dictation" feed that audio to the real engine instead of a sine wave.
    static func simulationUtterance() -> Utterance? {
        guard let path = ProcessInfo.processInfo.environment["LOCALFLOW_SIM_WAV"],
              let utterance = try? UtteranceLoader.load(contentsOf: URL(fileURLWithPath: path))
        else { return nil }
        return utterance
    }
}
