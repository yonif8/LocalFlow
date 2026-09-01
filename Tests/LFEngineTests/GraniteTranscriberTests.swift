import Foundation
import LFContracts
import Testing
@testable import LFEngine

// Integration tests. They require the Granite models to be present in the
// shared cache (~/Documents/huggingface by default); the first run downloads
// them. Fixture WAVs live in the repo's top-level Fixtures/ directory.

private var fixturesDirectory: URL {
    // Tests/LFEngineTests/… → repo root → Fixtures
    URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()  // GraniteTranscriberTests.swift
        .deletingLastPathComponent()  // LFEngineTests
        .deletingLastPathComponent()  // Tests
        .appendingPathComponent("Fixtures")
}

@Test func rejectsWrongSampleRate() async throws {
    let transcriber = GraniteTranscriber()
    let utterance = Utterance(samples: [Float](repeating: 0, count: 44_100), sampleRate: 44_100)
    await #expect(throws: EngineError.self) {
        _ = try await transcriber.transcribe(utterance)
    }
}

@Test func rejectsTooShortUtterance() async throws {
    let transcriber = GraniteTranscriber()
    let utterance = Utterance(samples: [Float](repeating: 0, count: 100), sampleRate: 16_000)
    await #expect(throws: EngineError.self) {
        _ = try await transcriber.transcribe(utterance)
    }
}

@Test func transcribesShortFixtureWithPunctuation() async throws {
    let wav = fixturesDirectory.appendingPathComponent("short_3s.wav")
    try #require(FileManager.default.fileExists(atPath: wav.path))
    let utterance = try UtteranceLoader.load(contentsOf: wav)
    #expect(utterance.sampleRate == 16_000)

    let transcriber = GraniteTranscriber()
    let (text, timings) = try await transcriber.transcribeWithTimings(utterance)
    #expect(text == "Hey, can you send me the report by Friday?")
    #expect(timings.modelLoad != nil)  // first call on this instance loads
    #expect(timings.inference > 0)

    // Second call must reuse the resident models.
    let (_, second) = try await transcriber.transcribeWithTimings(utterance)
    #expect(second.modelLoad == nil)
}

@Test func rawModeSkipsFormatter() async throws {
    let wav = fixturesDirectory.appendingPathComponent("short_3s.wav")
    try #require(FileManager.default.fileExists(atPath: wav.path))
    let utterance = try UtteranceLoader.load(contentsOf: wav)

    let transcriber = GraniteTranscriber(configuration: .init(punctuate: false))
    let (text, timings) = try await transcriber.transcribeWithTimings(utterance)
    #expect(text == "hey can you send me the report by friday")
    #expect(timings.formatting == 0)
}
