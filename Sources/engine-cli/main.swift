// engine-cli — transcribe a WAV with LocalFlow's Parakeet engine.
//
//   engine-cli <audio-file>             transcribe, print text + timing
//   engine-cli <audio-file> --bench [N] warm up, then N timed runs (default 10)
//
// First-ever run downloads the Parakeet models (~600 MB) into FluidAudio's
// Application Support cache.

import Foundation
import LFContracts
import LFEngine

func errPrint(_ message: String) {
    FileHandle.standardError.write(Data((message + "\n").utf8))
}

func fail(_ message: String) -> Never {
    errPrint("error: \(message)")
    exit(1)
}

func ms(_ seconds: TimeInterval) -> String {
    String(format: "%.0f ms", seconds * 1000)
}

var arguments = Array(CommandLine.arguments.dropFirst())
var bench = false
var benchRuns = 10
var audioPath: String?

var index = 0
while index < arguments.count {
    let argument = arguments[index]
    switch argument {
    case "--bench":
        bench = true
        if index + 1 < arguments.count, let n = Int(arguments[index + 1]), n > 0 {
            benchRuns = n
            index += 1
        }
    case "--help", "-h":
        print("usage: engine-cli <audio-file> [--bench [N]]")
        exit(0)
    default:
        if argument.hasPrefix("-") { fail("unknown option \(argument)") }
        guard audioPath == nil else { fail("multiple input files given") }
        audioPath = argument
    }
    index += 1
}

guard let audioPath else {
    fail("usage: engine-cli <audio-file> [--bench [N]]")
}

do {
    let utterance = try UtteranceLoader.load(contentsOf: URL(fileURLWithPath: audioPath))
    errPrint(String(
        format: "audio: %@ (%.2f s @ %.0f Hz, %d samples)",
        (audioPath as NSString).lastPathComponent, utterance.duration,
        utterance.sampleRate, utterance.samples.count))

    let transcriber = ParakeetTranscriber()
    ParakeetTranscriber.progressHandler = { progress in
        errPrint(String(
            format: "models: %@ %.0f%%", progress.phase, progress.fractionCompleted * 100))
    }
    let loadStart = Date()
    await transcriber.prepare()
    errPrint("model load: \(ms(Date().timeIntervalSince(loadStart)))")

    let runs = bench ? benchRuns : 1
    var latencies: [TimeInterval] = []
    var text = ""
    for _ in 1...runs {
        let start = Date()
        text = try await transcriber.transcribe(utterance)
        latencies.append(Date().timeIntervalSince(start))
    }
    let median = latencies.sorted()[latencies.count / 2]
    errPrint(String(
        format: "parakeet: median %@ over %d run(s), RTF %.3f",
        ms(median), runs, median / utterance.duration))
    print(text)
} catch {
    fail(String(describing: error))
}
