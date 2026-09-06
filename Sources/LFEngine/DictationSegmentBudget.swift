import Foundation

/// Initial calibration: generated speech on M1 Max, three warm runs each.
/// 12/24/36/48/60 seconds cost at most .503/.823/1.116/1.368/1.765
/// seconds for ASR + polish. Paste insertion observed at ~.34 seconds.
/// Reserve insertion time and headroom for carry-over words/load variation;
/// use the slowest measured processing/audio ratio, not an arbitrary duration.
public struct DictationSegmentBudget: Sendable {
    public var targetSeconds: Double = 2
    public var insertionSeconds: Double = 0.35
    public var headroomSeconds: Double = 0.5
    public var processingSecondsPerAudioSecond: Double = 0.503 / 12

    public init() {}

    /// Recheck the actual snapshot, not just a potentially delayed meter
    /// event, so speech that resumed while the UI was busy isn't cut mid-word.
    public static func hasQuietTail(_ samples: [Float], sampleRate: Double) -> Bool {
        let count = Int(sampleRate * 0.3)
        guard count > 0, samples.count >= count else { return false }
        let meanSquare = samples.suffix(count).reduce(Float(0)) { $0 + $1 * $1 } / Float(count)
        return meanSquare.isFinite && meanSquare.squareRoot() < 0.009
    }

    public var pauseSearchSeconds: Double {
        max(1, (targetSeconds - insertionSeconds - headroomSeconds)
            / max(0.001, processingSecondsPerAudioSecond))
    }

    /// Tighten the threshold after slower observations. Never make later
    /// chunks larger based on one unusually quick or mostly silent segment.
    public mutating func observe(audioSeconds: Double, processingSeconds: Double) {
        guard audioSeconds >= 1, processingSeconds.isFinite, processingSeconds > 0 else { return }
        processingSecondsPerAudioSecond = max(processingSecondsPerAudioSecond,
                                              processingSeconds / audioSeconds)
    }
}
