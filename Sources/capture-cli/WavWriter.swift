import Foundation

/// Minimal 16-bit PCM mono WAV writer (no AVFoundation dependency needed here).
enum WavWriter {
    static func write(samples: [Float], sampleRate: Int, to url: URL) throws {
        let numChannels: UInt16 = 1
        let bitsPerSample: UInt16 = 16
        let byteRate = UInt32(sampleRate) * UInt32(numChannels) * UInt32(bitsPerSample / 8)
        let blockAlign = UInt16(numChannels * bitsPerSample / 8)
        let dataSize = UInt32(samples.count * 2)

        var data = Data(capacity: 44 + samples.count * 2)
        data.append(contentsOf: Array("RIFF".utf8))
        appendLE(&data, UInt32(36 + dataSize))
        data.append(contentsOf: Array("WAVE".utf8))
        data.append(contentsOf: Array("fmt ".utf8))
        appendLE(&data, UInt32(16))            // fmt chunk size
        appendLE(&data, UInt16(1))             // PCM
        appendLE(&data, numChannels)
        appendLE(&data, UInt32(sampleRate))
        appendLE(&data, byteRate)
        appendLE(&data, blockAlign)
        appendLE(&data, bitsPerSample)
        data.append(contentsOf: Array("data".utf8))
        appendLE(&data, dataSize)

        for s in samples {
            let clamped = max(-1, min(1, s))
            let value = Int16(clamped * Float(Int16.max))
            appendLE(&data, UInt16(bitPattern: value))
        }
        try data.write(to: url)
    }

    private static func appendLE<T: FixedWidthInteger>(_ data: inout Data, _ value: T) {
        var le = value.littleEndian
        withUnsafeBytes(of: &le) { data.append(contentsOf: $0) }
    }
}
