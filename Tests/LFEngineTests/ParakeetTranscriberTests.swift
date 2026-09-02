import Foundation
import Testing
@testable import LFEngine
import LFContracts

@Suite("Parakeet engine")
struct ParakeetTranscriberTests {
    private var fixtureURL: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()  // LFEngineTests
            .deletingLastPathComponent()  // Tests
            .deletingLastPathComponent()  // repo root
            .appendingPathComponent("Fixtures/short_3s.wav")
    }

    @Test func loaderProduces16kMono() throws {
        let utterance = try UtteranceLoader.load(contentsOf: fixtureURL)
        #expect(utterance.sampleRate == 16_000)
        #expect(utterance.duration > 2.0 && utterance.duration < 4.0)
    }

    @Test func transcribesShortFixture() async throws {
        // Requires the Parakeet models to be cached (they are on dev machines);
        // first-ever run downloads ~600MB.
        let transcriber = ParakeetTranscriber()
        let text = try await transcriber.transcribe(
            try UtteranceLoader.load(contentsOf: fixtureURL))
        #expect(text.lowercased().contains("report"))
        #expect(text.lowercased().contains("friday"))
    }

    @Test func rejectsWrongSampleRate() async {
        let transcriber = ParakeetTranscriber()
        await #expect(throws: EngineError.self) {
            _ = try await transcriber.transcribe(
                Utterance(samples: Array(repeating: 0, count: 48_000), sampleRate: 48_000))
        }
    }
}
