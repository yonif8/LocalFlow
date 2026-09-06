import Foundation
import LFContracts
import LFEngine
import LFPolish

// Local-only benchmark. No microphone, insertion, transcript logging or downloads
// beyond the app's normal model loader. Feed a non-private recorded speech fixture.
guard CommandLine.arguments.count == 2 else {
    print("usage: dictation-bench speech.wav")
    exit(2)
}
let audio = try UtteranceLoader.load(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let engine = ParakeetTranscriber()
await engine.prepare()
let polisher = LocalPolisher(configuration: .init(timeout: 15, maxInputCharacters: 12000))
let context = PolishContext()
_ = await polisher.polish("warm up test", context: context)
for seconds in [12, 24, 36, 48, 60] {
    let count = min(audio.samples.count, seconds * 16000)
    let segment = Utterance(samples: Array(audio.samples.prefix(count)), sampleRate: 16000)
    for run in 1...3 {
        let start = Date()
        let raw = try await engine.transcribe(segment)
        let asr = Date().timeIntervalSince(start)
        let result = await polisher.polishDetailed(raw, context: context)
        print(String(format: "seconds=%.1f run=%d chars=%d asr=%.3f total=%.3f outcome=%@",
                     segment.duration, run, raw.count, asr,
                     Date().timeIntervalSince(start), String(describing: result.outcome)))
    }
}
