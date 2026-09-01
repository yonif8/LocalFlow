import Foundation
import GraniteMLX
import LFContracts

/// File-based `Utterance` loading, for the CLI and tests.
///
/// The live dictation path builds `Utterance`s directly from microphone
/// samples; this exists so tools can feed recorded audio through the exact
/// same `Transcriber` entry point. Decoding and resampling to 16 kHz mono are
/// delegated to GraniteMLX's audio frontend (AVFoundation with an ffmpeg
/// fallback), so anything AVFoundation can read works here.
public enum UtteranceLoader {
    /// Loads an audio file as a 16 kHz mono `Utterance`.
    public static func load(contentsOf url: URL) throws -> Utterance {
        let audio = try GraniteAudioInput.load(url: url, targetSampleRate: 16_000)
        return Utterance(samples: audio.samples, sampleRate: Double(audio.sampleRate))
    }
}
