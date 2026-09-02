import Foundation
import LFContracts
import LFEngine

// The speech engine: Parakeet TDT v3, the app's only ASR. Progress feeds the
// onboarding "Models" row on first run.
extension ParakeetTranscriber: PreparableTranscriber {}

enum EngineFactory {
    static func makeTranscriber() -> ParakeetTranscriber {
        ParakeetTranscriber.progressHandler = { progress in
            Task { @MainActor in
                ModelSetupState.shared.noteEngineProgress(progress)
            }
        }
        return ParakeetTranscriber()
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
