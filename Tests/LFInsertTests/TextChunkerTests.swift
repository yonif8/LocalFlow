import Testing
@testable import LFInsert

@Suite("TextChunker")
struct TextChunkerTests {
    @Test("empty string produces no chunks")
    func empty() {
        #expect(TextChunker.chunks("", maxUTF16PerChunk: 20).isEmpty)
    }

    @Test("short text is a single chunk")
    func short() {
        #expect(TextChunker.chunks("hello", maxUTF16PerChunk: 20) == ["hello"])
    }

    @Test("chunks rejoin to the original text", arguments: [
        "hello world this is a longer piece of dictated text, with punctuation!",
        "emoji 👍🏽 family 👨‍👩‍👧‍👦 flags 🇦🇺🇯🇵 mixed with ASCII",
        "accents: café naïve résumé — em-dash and “smart quotes”",
        "newlines\nand\ttabs survive chunking",
        "日本語のテキストと한국어 텍스트 and English",
        String(repeating: "a", count: 137),
    ])
    func roundTrip(text: String) {
        for size in [1, 3, 7, 20, 64] {
            let chunks = TextChunker.chunks(text, maxUTF16PerChunk: size)
            #expect(chunks.joined() == text, "size \(size)")
            #expect(chunks.allSatisfy { !$0.isEmpty }, "size \(size)")
        }
    }

    @Test("chunks respect the UTF-16 limit except for oversized single graphemes")
    func limitRespected() {
        let text = "abc👨‍👩‍👧‍👦def" // family emoji is 11 UTF-16 units
        let chunks = TextChunker.chunks(text, maxUTF16PerChunk: 4)
        #expect(chunks.joined() == text)
        for chunk in chunks {
            let units = chunk.utf16.count
            // Over-limit chunks are only allowed when they are one grapheme.
            #expect(units <= 4 || chunk.count == 1, "chunk '\(chunk)' has \(units) units")
        }
        // The family emoji must not have been split into surrogate halves.
        #expect(chunks.contains("👨‍👩‍👧‍👦"))
    }

    @Test("surrogate pairs are never split")
    func surrogates() {
        let text = "𝕏𝕐𝕑" // each is 2 UTF-16 units (non-BMP)
        let chunks = TextChunker.chunks(text, maxUTF16PerChunk: 1)
        #expect(chunks == ["𝕏", "𝕐", "𝕑"])
    }

    @Test("default-size chunking of long text stays at or under 20 units")
    func typicalSize() {
        let text = String(repeating: "the quick brown fox ", count: 25)
        let chunks = TextChunker.chunks(text, maxUTF16PerChunk: 20)
        #expect(chunks.joined() == text)
        #expect(chunks.allSatisfy { $0.utf16.count <= 20 })
    }
}
