// engine-cli — transcribe a WAV with the LocalFlow Granite engine.
//
//   engine-cli <audio-file>                 transcribe, print text + timing breakdown
//   engine-cli <audio-file> --bench [N]     warm up, then N timed runs (default 10);
//                                           report per-utterance latency stats
//   Options:
//     --no-punctuate    skip the punctuation/truecase formatter
//     --raw             also print the raw (unformatted) CTC text
//
// First run downloads the models (~550 MB speech + ~56 MB formatter) into the
// shared Granite-MLX cache; progress is printed to stderr.

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

// MARK: - Argument parsing

var arguments = Array(CommandLine.arguments.dropFirst())
var bench = false
var benchRuns = 10
var punctuate = true
var showRaw = false
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
    case "--no-punctuate":
        punctuate = false
    case "--raw":
        showRaw = true
    case "--help", "-h":
        print("usage: engine-cli <audio-file> [--bench [N]] [--no-punctuate] [--raw]")
        exit(0)
    default:
        if argument.hasPrefix("-") { fail("unknown option \(argument)") }
        guard audioPath == nil else { fail("multiple input files given") }
        audioPath = argument
    }
    index += 1
}

guard let audioPath else {
    fail("usage: engine-cli <audio-file> [--bench [N]] [--no-punctuate] [--raw]")
}
let audioURL = URL(fileURLWithPath: audioPath)

// MARK: - Run

do {
    let utterance = try UtteranceLoader.load(contentsOf: audioURL)
        errPrint(String(
            format: "audio: %@ (%.2f s @ %.0f Hz, %d samples)",
            audioURL.lastPathComponent, utterance.duration,
            utterance.sampleRate, utterance.samples.count))

        let transcriber = GraniteTranscriber(configuration: .init(
            punctuate: punctuate,
            progressHandler: { progress in
                errPrint(String(
                    format: "model %@: %@ %.0f%%",
                    progress.repositoryID, progress.phase,
                    progress.fractionCompleted * 100))
            }
        ))

        if bench {
            errPrint("preparing models + warm-up run…")
            let load = try await transcriber.prepare(warmRun: true)
            if let load { errPrint("model load: \(ms(load))") }

            var latencies: [TimeInterval] = []
            var lastText = ""
            for run in 1...benchRuns {
                let (text, timings) = try await transcriber.transcribeWithTimings(utterance)
                lastText = text
                latencies.append(timings.total)
                errPrint(String(
                    format: "run %2d/%d: total %@ (inference %@, formatting %@)",
                    run, benchRuns, ms(timings.total),
                    ms(timings.inference), ms(timings.formatting)))
            }
            let sorted = latencies.sorted()
            let median = sorted[sorted.count / 2]
            let mean = latencies.reduce(0, +) / Double(latencies.count)
            print("transcript: \(lastText)")
            print(String(
                format: "bench (%d runs, %.2f s audio, warm): median %@  mean %@  min %@  max %@  RTF %.3f",
                benchRuns, utterance.duration,
                ms(median), ms(mean), ms(sorted.first!), ms(sorted.last!),
                median / utterance.duration))
        } else {
            let (text, timings) = try await transcriber.transcribeWithTimings(utterance)
            print(text)
            errPrint("--")
            if let load = timings.modelLoad {
                errPrint("model load:  \(ms(load))")
            }
            errPrint("inference:   \(ms(timings.inference))")
            if punctuate {
                errPrint("formatting:  \(ms(timings.formatting))")
            }
            errPrint(String(
                format: "total:       %@ (%.2f s audio, RTF %.3f excl. load)",
                ms(timings.total), utterance.duration,
                (timings.inference + timings.formatting) / utterance.duration))
            if showRaw {
                let rawTranscriber = GraniteTranscriber(configuration: .init(punctuate: false))
                let (raw, _) = try await rawTranscriber.transcribeWithTimings(utterance)
                errPrint("raw: \(raw)")
            }
        }
} catch {
    errPrint("error: \(error)")
    exit(1)
}
