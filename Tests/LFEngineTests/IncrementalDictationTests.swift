import Foundation
import Testing
import LFContracts
@testable import LFEngine

private actor SegmentTranscriber: Transcriber {
    var calls = 0
    var active = 0
    var maximumActive = 0
    var transcripts: [String]
    var fails = false
    init(_ transcripts: [String], fails: Bool = false) {
        self.transcripts = transcripts
        self.fails = fails
    }
    func transcribe(_ utterance: Utterance) async throws -> String {
        active += 1
        maximumActive = max(maximumActive, active)
        defer { active -= 1 }
        calls += 1
        try await Task.sleep(for: .milliseconds(10))
        if fails { throw CancellationError() }
        return transcripts.removeFirst()
    }
}

private actor TransformInputs {
    var values: [String] = []
    func apply(_ input: String) -> String { values.append(input); return input }
}

@Suite("Incremental dictation")
struct IncrementalDictationTests {
    let audio = Utterance(samples: Array(repeating: 0.1, count: 16000), sampleRate: 16000)
    let silence = Utterance(samples: Array(repeating: 0, count: 16000), sampleRate: 16000)

    @Test func processesInOrderAndCarriesLastSentence() async throws {
        let transcriber = SegmentTranscriber(["First sentence. Second sentence.", "Third sentence.", "Final words."])
        let inputs = TransformInputs()
        let stream = IncrementalDictation(transcriber: transcriber) { await inputs.apply($0) }
        await stream.append(audio)
        await stream.append(audio)
        let text = try await stream.finish(audio)
        #expect(text == "First sentence. Second sentence. Third sentence. Final words.")
        #expect(await inputs.values == ["First sentence.", "Second sentence.", "Third sentence. Final words."])
        #expect(await transcriber.maximumActive == 1)
    }

    @Test func quietReleaseFlushesHeldWordsWithoutASR() async throws {
        let transcriber = SegmentTranscriber(["Keep all these words."])
        let stream = IncrementalDictation(transcriber: transcriber) { $0 }
        await stream.append(audio)
        #expect(try await stream.finish(silence) == "Keep all these words.")
        #expect(await transcriber.calls == 1)
    }

    @Test func shortDictationIsOnePass() async throws {
        let transcriber = SegmentTranscriber(["Short message."])
        let stream = IncrementalDictation(transcriber: transcriber) { $0 }
        #expect(try await stream.finish(audio) == "Short message.")
        #expect(await transcriber.calls == 1)
    }

    @Test func failedSegmentNeverProducesPartialSuccess() async {
        let transcriber = SegmentTranscriber([], fails: true)
        let stream = IncrementalDictation(transcriber: transcriber) { $0 }
        await stream.append(audio)
        await #expect(throws: CancellationError.self) { try await stream.finish(audio) }
        #expect(await transcriber.calls == 1)
    }

    @Test func cancelledSessionCannotFinishOrLeakIntoNewOne() async throws {
        let transcriber = SegmentTranscriber(["New message."])
        let old = IncrementalDictation(transcriber: transcriber) { $0 }
        await old.cancel()
        await old.append(audio)
        await #expect(throws: CancellationError.self) { try await old.finish(audio) }
        let fresh = IncrementalDictation(transcriber: transcriber) { $0 }
        #expect(try await fresh.finish(audio) == "New message.")
        #expect(await transcriber.calls == 1)
    }

    @Test func measuredBudgetTightensOnlyForSlowerWork() {
        var budget = DictationSegmentBudget()
        let initial = budget.pauseSearchSeconds
        #expect(initial > 27 && initial < 28)
        budget.observe(audioSeconds: 30, processingSeconds: 0.2)
        #expect(budget.pauseSearchSeconds == initial)
        budget.observe(audioSeconds: 30, processingSeconds: 3)
        #expect(budget.pauseSearchSeconds < initial)
        budget.observe(audioSeconds: 0, processingSeconds: .infinity)
        #expect(budget.pauseSearchSeconds.isFinite)
    }

    @Test func staleQuietMeterCannotSplitResumedSpeech() {
        #expect(DictationSegmentBudget.hasQuietTail(silence.samples, sampleRate: 16000))
        #expect(!DictationSegmentBudget.hasQuietTail(audio.samples, sampleRate: 16000))
        #expect(!DictationSegmentBudget.hasQuietTail([], sampleRate: 16000))
        let resumed = Array(repeating: Float(0), count: 16000) + Array(repeating: Float(0.1), count: 1600)
        #expect(!DictationSegmentBudget.hasQuietTail(resumed, sampleRate: 16000))
    }
}
