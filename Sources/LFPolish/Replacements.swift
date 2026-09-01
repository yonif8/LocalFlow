import Foundation

// MARK: - Stage 1: deterministic personal-dictionary replacements.

/// One user-defined replacement pair: a spoken form and the written form it
/// should become (e.g. "echidna cams" -> "EchidnaCams").
public struct ReplacementRule: Codable, Sendable, Equatable {
    public var spoken: String
    public var written: String

    public init(spoken: String, written: String) {
        self.spoken = spoken
        self.written = written
    }
}

/// The user's personal dictionary. Persisted as JSON at a caller-supplied URL.
public struct PersonalDictionary: Codable, Sendable, Equatable {
    public var rules: [ReplacementRule]
    /// Built-in spoken-punctuation commands ("comma", "period", ...). OFF by default.
    public var spokenPunctuationEnabled: Bool

    public init(rules: [ReplacementRule] = [], spokenPunctuationEnabled: Bool = false) {
        self.rules = rules
        self.spokenPunctuationEnabled = spokenPunctuationEnabled
    }

    public static func load(from url: URL) throws -> PersonalDictionary {
        let data = try Data(contentsOf: url)
        return try JSONDecoder().decode(PersonalDictionary.self, from: data)
    }

    public func save(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(self)
        try data.write(to: url, options: .atomic)
    }
}

/// Applies a `PersonalDictionary` to text. Deterministic; always safe to run.
///
/// Matching is case-insensitive and whole-word (word boundaries on both
/// sides); multiple spaces between words of a spoken form are tolerated.
/// All rules are matched against the *original* text in a single pass —
/// overlapping candidates are resolved longest-match-first — so one rule's
/// output is never re-matched by another rule and `apply` is idempotent for
/// ordinary dictionaries.
public struct ReplacementEngine: Sendable {
    public let dictionary: PersonalDictionary
    private let compiled: [CompiledRule]

    private struct CompiledRule: @unchecked Sendable {
        let regex: NSRegularExpression
        let written: String
        let spokenLength: Int
        let priority: Int
    }

    public init(dictionary: PersonalDictionary) {
        self.dictionary = dictionary
        // Longest spoken form first so ties in overlap resolution favor it.
        let ordered = dictionary.rules
            .filter { !$0.spoken.trimmingCharacters(in: .whitespaces).isEmpty }
            .sorted { $0.spoken.count > $1.spoken.count }
        self.compiled = ordered.enumerated().compactMap { index, rule in
            guard let regex = Self.wholeWordRegex(for: rule.spoken) else { return nil }
            return CompiledRule(
                regex: regex,
                written: rule.written,
                spokenLength: rule.spoken.count,
                priority: index
            )
        }
    }

    private static func wholeWordRegex(for spoken: String) -> NSRegularExpression? {
        let words = spoken
            .split(whereSeparator: { $0.isWhitespace })
            .map { NSRegularExpression.escapedPattern(for: String($0)) }
        guard !words.isEmpty else { return nil }
        let pattern = "\\b" + words.joined(separator: "\\s+") + "\\b"
        return try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive])
    }

    public func apply(to text: String) -> String {
        var result = text
        if !compiled.isEmpty {
            result = applyRules(to: result)
        }
        if dictionary.spokenPunctuationEnabled {
            result = SpokenPunctuation.apply(to: result)
        }
        return result
    }

    private func applyRules(to text: String) -> String {
        let ns = text as NSString
        let full = NSRange(location: 0, length: ns.length)

        struct Candidate {
            let range: NSRange
            let written: String
            let priority: Int
        }

        var candidates: [Candidate] = []
        for rule in compiled {
            rule.regex.enumerateMatches(in: text, options: [], range: full) { match, _, _ in
                if let match {
                    candidates.append(
                        Candidate(range: match.range, written: rule.written, priority: rule.priority))
                }
            }
        }
        guard !candidates.isEmpty else { return text }

        // Earliest start wins; on identical/overlapping starts, longest match
        // wins; then the higher-priority (longer spoken form) rule.
        candidates.sort {
            if $0.range.location != $1.range.location {
                return $0.range.location < $1.range.location
            }
            if $0.range.length != $1.range.length {
                return $0.range.length > $1.range.length
            }
            return $0.priority < $1.priority
        }

        var accepted: [Candidate] = []
        var cursor = 0
        for candidate in candidates {
            if candidate.range.location >= cursor {
                accepted.append(candidate)
                cursor = candidate.range.location + candidate.range.length
            }
        }

        let output = NSMutableString(string: text)
        for candidate in accepted.reversed() {
            let matched = ns.substring(with: candidate.range)
            let replacement = Self.adaptCase(written: candidate.written, matched: matched)
            output.replaceCharacters(in: candidate.range, with: replacement)
        }
        return output as String
    }

    /// Case-preserving apply "where sensible": if the written form itself
    /// contains any uppercase, the author chose its casing — use it verbatim.
    /// Otherwise mirror the matched text: ALL-CAPS match -> uppercased written
    /// form; Capitalized match (e.g. sentence start) -> capitalized written form.
    static func adaptCase(written: String, matched: String) -> String {
        guard !written.contains(where: { $0.isUppercase }) else { return written }
        let letters = matched.filter(\.isLetter)
        guard let firstLetter = letters.first else { return written }
        if letters.count > 1, letters.allSatisfy(\.isUppercase) {
            return written.uppercased()
        }
        if firstLetter.isUppercase {
            return written.prefix(1).uppercased() + written.dropFirst()
        }
        return written
    }
}

/// Built-in spoken punctuation commands. Deliberately small; OFF by default
/// because words like "period" and "colon" are common English words.
enum SpokenPunctuation {
    /// Ordered longest-phrase-first so "question mark" wins over any
    /// hypothetical "mark" rule and "new paragraph" wins over "new line".
    static let commands: [(phrase: String, replacement: String, consumesTrailingSpace: Bool)] = [
        ("new paragraph", "\n\n", true),
        ("question mark", "?", false),
        ("exclamation mark", "!", false),
        ("exclamation point", "!", false),
        ("new line", "\n", true),
        ("semicolon", ";", false),
        ("period", ".", false),
        ("comma", ",", false),
        ("colon", ":", false),
    ]

    static func apply(to text: String) -> String {
        var result = text
        for command in commands {
            let words = command.phrase.split(separator: " ")
                .map { NSRegularExpression.escapedPattern(for: String($0)) }
            var pattern = "\\s*\\b" + words.joined(separator: "\\s+") + "\\b"
            if command.consumesTrailingSpace { pattern += "[ \\t]*" }
            guard let regex = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive])
            else { continue }
            let range = NSRange(location: 0, length: (result as NSString).length)
            result = regex.stringByReplacingMatches(
                in: result, options: [], range: range,
                withTemplate: NSRegularExpression.escapedTemplate(for: command.replacement))
        }
        return result
    }
}
