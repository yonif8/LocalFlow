import Foundation

/// Splits text into chunks for `keyboardSetUnicodeString` typing without ever
/// splitting a grapheme cluster (so surrogate pairs, emoji ZWJ sequences and
/// combining marks stay intact).
public enum TextChunker {
    /// Chunks `text` so each chunk is at most `maxUTF16PerChunk` UTF-16 code
    /// units, except when a single grapheme cluster alone exceeds the limit —
    /// then it is emitted as its own (oversized) chunk rather than split.
    /// Joining the chunks always reproduces `text` exactly.
    public static func chunks(_ text: String, maxUTF16PerChunk: Int) -> [String] {
        precondition(maxUTF16PerChunk > 0, "chunk size must be positive")
        guard !text.isEmpty else { return [] }

        var result: [String] = []
        var current = ""
        var currentUnits = 0

        for character in text {
            let units = character.utf16.count
            if currentUnits > 0 && currentUnits + units > maxUTF16PerChunk {
                result.append(current)
                current = ""
                currentUnits = 0
            }
            current.append(character)
            currentUnits += units
        }
        if !current.isEmpty {
            result.append(current)
        }
        return result
    }
}
