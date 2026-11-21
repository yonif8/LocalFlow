import Foundation

/// Initial calibration: generated speech on M1 Max, three warm runs each.
/// 12/24/36/48/60 seconds cost at most .503/.823/1.116/1.368/1.765
/// seconds for ASR + polish. Paste insertion observed at ~.34 seconds.
/// Reserve insertion time and headroom for carry-over words/load variation.
///
/// Accuracy outranks the latency aim: every background chunk is transcribed
/// without its neighbours' context, so chunks are never shorter than
/// `minimumPauseSearchSeconds`. A slow machine gets a slower release, never
/// fragmented transcription. (1.4.5–1.4.7 let one slow run ratchet the
/// threshold down for the life of the process; short chunks then measured
/// "slower" because fixed overhead dominated them, collapsing it to 1 s.)
public struct DictationSegmentBudget: Sendable {
    public static let minimumPauseSearchSeconds: Double = 20
    /// Shorter observations are dominated by fixed per-call overhead (polish,
    /// model dispatch) and say nothing about the per-second cost.
    public static let minimumObservedAudioSeconds: Double = 12
    public static let recentObservationLimit = 5
    public static let calibratedSecondsPerAudioSecond: Double = 0.503 / 12

    public var targetSeconds: Double = 2
    public var insertionSeconds: Double = 0.35
    public var headroomSeconds: Double = 0.5
    private var recentRates: [Double] = []

    public init() {}

    /// Recheck the actual snapshot, not just a potentially delayed meter
    /// event, so speech that resumed while the UI was busy isn't cut mid-word.
    public static func hasQuietTail(_ samples: [Float], sampleRate: Double) -> Bool {
        let count = Int(sampleRate * 0.3)
        guard count > 0, samples.count >= count else { return false }
        let meanSquare = samples.suffix(count).reduce(Float(0)) { $0 + $1 * $1 } / Float(count)
        return meanSquare.isFinite && meanSquare.squareRoot() < 0.009
    }

    /// Median of recent observations, never faster than the calibrated seed.
    /// The median forgets a single cold-start or polish-timeout outlier.
    public var processingSecondsPerAudioSecond: Double {
        guard !recentRates.isEmpty else { return Self.calibratedSecondsPerAudioSecond }
        let sorted = recentRates.sorted()
        let mid = sorted.count / 2
        let median = sorted.count.isMultiple(of: 2) ? (sorted[mid - 1] + sorted[mid]) / 2 : sorted[mid]
        return max(Self.calibratedSecondsPerAudioSecond, median)
    }

    public var pauseSearchSeconds: Double {
        max(Self.minimumPauseSearchSeconds,
            (targetSeconds - insertionSeconds - headroomSeconds)
                / max(0.001, processingSecondsPerAudioSecond))
    }

    public mutating func observe(audioSeconds: Double, processingSeconds: Double) {
        guard audioSeconds.isFinite, audioSeconds >= Self.minimumObservedAudioSeconds,
              processingSeconds.isFinite, processingSeconds > 0 else { return }
        recentRates.append(processingSeconds / audioSeconds)
        if recentRates.count > Self.recentObservationLimit {
            recentRates.removeFirst(recentRates.count - Self.recentObservationLimit)
        }
    }
}
