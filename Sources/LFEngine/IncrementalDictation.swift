import Foundation
import NaturalLanguage
import LFContracts

/// Serial processing without blocking the capture event pump. Every append
/// depends on its predecessor; failures propagate, never omit a segment.
public actor IncrementalDictation {
    public typealias Transform = @Sendable (String) async throws -> String
    private struct Result: Sendable {
        var completed = ""
        var pending = ""
    }
    private let transcriber: any Transcriber
    private let transform: Transform
    private let timing: @Sendable (Double, Double) async -> Void
    private var tail: Task<Result, Error>?
    private var tasks: [Task<Result, Error>] = []
    private var closed = false

    public init(transcriber: any Transcriber,
                timing: @escaping @Sendable (Double, Double) async -> Void = { _, _ in },
                transform: @escaping Transform) {
        self.transcriber = transcriber
        self.transform = transform
        self.timing = timing
    }

    public func append(_ audio: Utterance) {
        guard !closed else { return }
        enqueue(audio, final: false)
    }

    public func finish(_ audio: Utterance) async throws -> String {
        guard !closed else { throw CancellationError() }
        closed = true
        let task = enqueue(audio, final: true)
        defer { tasks.removeAll(); tail = nil }
        return try await task.value.completed
    }

    public func cancel() {
        closed = true
        tasks.forEach { $0.cancel() }
        tasks.removeAll()
        tail = nil
    }

    @discardableResult
    private func enqueue(_ audio: Utterance, final: Bool) -> Task<Result, Error> {
        let previous = tail
        let transcriber = self.transcriber
        let transform = self.transform
        let timing = self.timing
        let task = Task<Result, Error> {
            var result = try await previous?.value ?? Result()
            try Task.checkCancellation()
            let started = Date()
            // A quiet release tail is not another utterance. Preserve any
            // speech, including very short last words, but skip digital silence.
            let raw: String
            if audio.samples.count > 256 && audio.samples.contains(where: { abs($0) > 0.0001 }) {
                raw = try await transcriber.transcribe(audio)
            } else {
                raw = ""
            }
            try Task.checkCancellation()
            let combined = Self.join(result.pending, raw)
            let parts = final ? (combined, "") : Self.holdLastSentence(combined)
            if !parts.0.isEmpty {
                let polished = try await transform(parts.0)
                try Task.checkCancellation()
                result.completed = Self.join(result.completed, polished)
            }
            result.pending = parts.1
            if !final { await timing(audio.duration, Date().timeIntervalSince(started)) }
            return result
        }
        tail = task
        tasks.append(task)
        return task
    }

    static func join(_ left: String, _ right: String) -> String {
        [left, right].filter { !$0.isEmpty }.joined(separator: " ")
    }

    /// Always retain the last sentence, even when ASR added a period at a
    /// pause. Its words are polished together with the following segment.
    static func holdLastSentence(_ text: String) -> (String, String) {
        let tokenizer = NLTokenizer(unit: .sentence)
        tokenizer.string = text
        var last: Range<String.Index>?
        tokenizer.enumerateTokens(in: text.startIndex..<text.endIndex) { range, _ in
            last = range
            return true
        }
        guard let last else { return ("", text) }
        return (String(text[..<last.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines),
                String(text[last.lowerBound...]).trimmingCharacters(in: .whitespacesAndNewlines))
    }
}
