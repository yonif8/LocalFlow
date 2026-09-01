@preconcurrency import AVFoundation
import Foundation

/// AVAudioEngine microphone pipeline producing 16 kHz mono Float32 samples.
///
/// The input tap runs at the device's native format and an AVAudioConverter
/// resamples/downmixes to 16 kHz mono. With `keepWarm`, the engine keeps
/// running between utterances and the tap simply discards buffers while idle —
/// this avoids losing the first syllable and mitigates the 1-2 s mic spin-up
/// on AirPods/Bluetooth inputs.
final class FlagBox: @unchecked Sendable {
    var value = false
}

public final class MicRecorder: @unchecked Sendable {
    public static let targetSampleRate: Double = 16_000

    private let engine = AVAudioEngine()
    private let lock = NSLock()
    private var converter: AVAudioConverter?
    private var samples: [Float] = []
    private var recording = false
    private var keepWarm = true
    private var prepared = false

    /// Called from the audio thread with an input level in 0...1 while recording.
    public var onLevel: (@Sendable (Float) -> Void)?

    private let targetFormat = AVAudioFormat(
        commonFormat: .pcmFormatFloat32,
        sampleRate: MicRecorder.targetSampleRate,
        channels: 1,
        interleaved: false
    )!

    public init() {}

    /// Install the input tap; if `keepWarm` also start the engine immediately.
    public func prepare(keepWarm: Bool) throws {
        lock.lock()
        defer { lock.unlock() }
        guard !prepared else { return }
        self.keepWarm = keepWarm

        let input = engine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw CaptureError.noInputDevice
        }

        // 4096 frames ≈ 85 ms at 48 kHz — cheap, and frequent enough for metering.
        input.installTap(onBus: 0, bufferSize: 4096, format: inputFormat) { [weak self] buffer, _ in
            self?.process(buffer: buffer)
        }
        engine.prepare()
        prepared = true

        if keepWarm {
            try startEngineLocked()
        }
    }

    /// Begin accumulating samples. Starts the engine now if not kept warm.
    public func beginRecording() throws {
        lock.lock()
        defer { lock.unlock() }
        samples.removeAll(keepingCapacity: true)
        recording = true
        if !engine.isRunning {
            try startEngineLocked()
        }
    }

    /// Stop accumulating and return everything captured since `beginRecording`.
    public func endRecording() -> [Float] {
        lock.lock()
        defer { lock.unlock() }
        recording = false
        let out = samples
        samples.removeAll(keepingCapacity: true)
        if !keepWarm { engine.pause() }
        return out
    }

    /// Discard the in-flight recording.
    public func cancelRecording() {
        lock.lock()
        defer { lock.unlock() }
        recording = false
        samples.removeAll(keepingCapacity: true)
        if !keepWarm { engine.pause() }
    }

    public func shutdown() {
        lock.lock()
        defer { lock.unlock() }
        recording = false
        if prepared { engine.inputNode.removeTap(onBus: 0) }
        engine.stop()
        prepared = false
        converter = nil
        samples.removeAll()
    }

    // MARK: - Private

    private func startEngineLocked() throws {
        do {
            try engine.start()
        } catch {
            throw CaptureError.audioEngineFailed(String(describing: error))
        }
    }

    /// Audio-thread buffer handler: convert to 16 kHz mono and accumulate.
    private func process(buffer: AVAudioPCMBuffer) {
        lock.lock()
        let isRecording = recording
        lock.unlock()
        guard isRecording else { return } // warm-idle: discard immediately

        // (Re)create the converter if the device format changed (e.g. new input device).
        if converter == nil || converter!.inputFormat != buffer.format {
            converter = AVAudioConverter(from: buffer.format, to: targetFormat)
        }
        guard let converter else { return }

        let ratio = targetFormat.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 64
        guard let outBuffer = AVAudioPCMBuffer(pcmFormat: targetFormat, frameCapacity: capacity) else { return }

        // The input block runs synchronously inside `convert`; the box just
        // satisfies strict-concurrency checking for the captured flag.
        let consumed = FlagBox()
        var error: NSError?
        let status = converter.convert(to: outBuffer, error: &error) { _, outStatus in
            if consumed.value {
                outStatus.pointee = .noDataNow
                return nil
            }
            consumed.value = true
            outStatus.pointee = .haveData
            return buffer
        }
        guard status != .error, error == nil,
              outBuffer.frameLength > 0,
              let channel = outBuffer.floatChannelData?[0] else { return }

        let count = Int(outBuffer.frameLength)
        let chunk = UnsafeBufferPointer(start: channel, count: count)

        // RMS → dB → 0...1 (floor at -50 dBFS).
        var sumSquares: Float = 0
        for s in chunk { sumSquares += s * s }
        let rms = (sumSquares / Float(count)).squareRoot()
        let db = 20 * log10(max(rms, 1e-7))
        let level = min(1, max(0, (db + 50) / 50))

        lock.lock()
        if recording { samples.append(contentsOf: chunk) }
        let stillRecording = recording
        lock.unlock()

        if stillRecording { onLevel?(level) }
    }
}
