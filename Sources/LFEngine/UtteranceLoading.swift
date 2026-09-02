@preconcurrency import AVFoundation
import Foundation
import LFContracts

/// Loads an audio file as a 16 kHz mono `Utterance` (pure AVFoundation).
public enum UtteranceLoader {
    public static func load(contentsOf url: URL) throws -> Utterance {
        let file: AVAudioFile
        do {
            file = try AVAudioFile(forReading: url)
        } catch {
            throw EngineError.audioLoadFailed(String(describing: error))
        }

        guard let targetFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32, sampleRate: 16_000, channels: 1, interleaved: false)
        else {
            throw EngineError.audioLoadFailed("could not create 16kHz mono format")
        }

        guard let converter = AVAudioConverter(from: file.processingFormat, to: targetFormat) else {
            throw EngineError.audioLoadFailed("no converter from \(file.processingFormat)")
        }

        let sourceCapacity = AVAudioFrameCount(8192)
        let ratio = targetFormat.sampleRate / file.processingFormat.sampleRate
        let targetCapacity = AVAudioFrameCount((Double(sourceCapacity) * ratio).rounded(.up) + 32)

        guard let inBuffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: sourceCapacity),
              let outBuffer = AVAudioPCMBuffer(pcmFormat: targetFormat, frameCapacity: targetCapacity)
        else {
            throw EngineError.audioLoadFailed("buffer allocation failed")
        }

        var samples: [Float] = []
        var reachedEnd = false
        while !reachedEnd {
            do {
                try file.read(into: inBuffer)
            } catch {
                throw EngineError.audioLoadFailed(String(describing: error))
            }
            if inBuffer.frameLength == 0 { break }
            reachedEnd = file.framePosition >= file.length

            var fed = false
            var conversionError: NSError?
            outBuffer.frameLength = 0
            let status = converter.convert(to: outBuffer, error: &conversionError) { _, outStatus in
                if fed {
                    outStatus.pointee = .noDataNow
                    return nil
                }
                fed = true
                outStatus.pointee = .haveData
                return inBuffer
            }
            if let conversionError {
                throw EngineError.audioLoadFailed(String(describing: conversionError))
            }
            if status == .error {
                throw EngineError.audioLoadFailed("conversion failed")
            }
            if outBuffer.frameLength > 0, let channel = outBuffer.floatChannelData?[0] {
                samples.append(contentsOf: UnsafeBufferPointer(start: channel, count: Int(outBuffer.frameLength)))
            }
        }

        // Drain the converter's tail.
        var drainError: NSError?
        outBuffer.frameLength = 0
        let drainStatus = converter.convert(to: outBuffer, error: &drainError) { _, outStatus in
            outStatus.pointee = .endOfStream
            return nil
        }
        if drainStatus != .error, outBuffer.frameLength > 0, let channel = outBuffer.floatChannelData?[0] {
            samples.append(contentsOf: UnsafeBufferPointer(start: channel, count: Int(outBuffer.frameLength)))
        }

        return Utterance(samples: samples, sampleRate: 16_000)
    }
}
