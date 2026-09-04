import Foundation

/// A canonical spelling learned from a high-confidence screen-context correction.
public struct LearnedTerm: Codable, Sendable, Equatable, Identifiable {
    public var id: UUID
    public var canonical: String
    public var aliases: [String]
    public var useCount: Int
    public var createdAt: Date
    public var lastUsedAt: Date
    public var sourceBundleID: String?

    public init(
        id: UUID = UUID(), canonical: String, aliases: [String] = [],
        useCount: Int = 1, createdAt: Date = Date(), lastUsedAt: Date = Date(),
        sourceBundleID: String? = nil
    ) {
        self.id = id
        self.canonical = canonical
        self.aliases = aliases
        self.useCount = useCount
        self.createdAt = createdAt
        self.lastUsedAt = lastUsedAt
        self.sourceBundleID = sourceBundleID
    }
}

public struct TerminologyMatch: Sendable, Equatable {
    public enum Source: String, Sendable { case screen, learned }
    public let heard: String
    public let canonical: String
    public let confidence: Double
    public let source: Source
}

public struct TerminologyCorrectionResult: Sendable, Equatable {
    public let text: String
    public let matches: [TerminologyMatch]
}

/// Extracts likely names and technical tokens without knowing any product names.
/// Only generic shape signals are used: mixed case, acronyms, digits, technical
/// punctuation, and capitalized names/phrases.
public enum ScreenTermExtractor {
    private static let tokenRegex = try! NSRegularExpression(
        pattern: #"[\p{L}\p{N}][\p{L}\p{N}._+#/\-]*"#)
    private static let sensitiveRegex = try! NSRegularExpression(
        pattern: #"(?:https?://|\b\S+@)\S+|\b[A-Za-z0-9_-]{24,}\b"#,
        options: [.caseInsensitive])
    private static let commonCapitalized: Set<String> = [
        "about", "account", "add", "back", "cancel", "close", "configure", "connect", "continue",
        "copy", "delete", "done", "edit", "file", "find", "general", "help",
        "home", "learn", "menu", "new", "next", "open", "preferences",
        "remove", "save", "search", "settings", "share", "tools", "view",
        "window", "yes", "no", "today", "tomorrow"
    ]

    public static func extract(from visibleStrings: [String], limit: Int = 120) -> [String] {
        struct Ranked { let term: String; let score: Int }
        var best: [String: Ranked] = [:]

        func offer(_ term: String, score: Int) {
            let cleaned = term.trimmingCharacters(in: .whitespacesAndNewlines)
            guard isSafe(cleaned) else { return }
            let key = normalized(cleaned)
            guard !key.isEmpty, best[key]?.score ?? -1 < score else { return }
            best[key] = Ranked(term: cleaned, score: score)
        }

        for text in visibleStrings.prefix(400) {
            let original = text as NSString
            let sanitized = sensitiveRegex.stringByReplacingMatches(
                in: text,
                range: NSRange(location: 0, length: original.length),
                withTemplate: " ")
            let ns = sanitized as NSString
            let matches = tokenRegex.matches(
                in: sanitized, range: NSRange(location: 0, length: ns.length))
            let tokens = matches.map { ns.substring(with: $0.range) }
            for token in tokens {
                if distinctive(token) { offer(token, score: 4) }
                else if titleCased(token), !commonCapitalized.contains(token.lowercased()) {
                    offer(token, score: 2)
                }
                if token.contains("/") {
                    let components = token.split(separator: "/").map(String.init)
                    for component in components where distinctive(component) {
                        offer(component, score: 4)
                    }
                    // Absolute paths are too noisy and often exceed the safety
                    // cap. The final two or three components are what people
                    // naturally dictate and uniquely identify the visible file.
                    if components.count >= 2 {
                        offer(components.suffix(2).joined(separator: "/"), score: 7)
                    }
                    if components.count >= 3 {
                        offer(components.suffix(3).joined(separator: "/"), score: 6)
                    }
                }
            }

            // Preserve proper-name phrases such as "Jane Smith" and product
            // names such as "Visual Studio Code" as single correction units.
            var index = 0
            while index < tokens.count {
                guard titleCased(tokens[index]) else { index += 1; continue }
                var end = index
                while end + 1 < tokens.count, end - index < 3,
                      titleCased(tokens[end + 1]) { end += 1 }
                if end > index {
                    for lower in index..<end
                    where !commonCapitalized.contains(tokens[lower].lowercased()) {
                        for upper in (lower + 1)...end {
                            offer(tokens[lower...upper].joined(separator: " "),
                                  score: 3 + upper - lower)
                        }
                    }
                }
                index = end + 1
            }
        }
        return best.values.sorted {
            $0.score == $1.score ? $0.term.count > $1.term.count : $0.score > $1.score
        }.prefix(max(0, limit)).map(\.term)
    }

    public static func normalized(_ value: String) -> String {
        value.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: .current)
            .filter { $0.isLetter || $0.isNumber }
            .lowercased()
    }

    private static func titleCased(_ token: String) -> Bool {
        guard token.count >= 3, let first = token.first, first.isUppercase else { return false }
        return token.dropFirst().contains(where: \.isLowercase)
    }

    private static func distinctive(_ token: String) -> Bool {
        let letters = token.filter(\.isLetter)
        let hasUpper = letters.contains(where: \.isUppercase)
        let hasLower = letters.contains(where: \.isLowercase)
        let innerUpper = token.dropFirst().contains(where: \.isUppercase)
        let acronym = letters.count >= 2 && hasUpper && !hasLower
        let alphanumeric = token.contains(where: \.isNumber) && !letters.isEmpty
        let technicalPunctuation = token.contains { "._+#/-".contains($0) }
        return innerUpper || acronym || alphanumeric || technicalPunctuation
    }

    private static func isSafe(_ term: String) -> Bool {
        guard (2...80).contains(term.count), !term.contains("@"),
              !term.lowercased().contains("://") else { return false }
        let alphanumerics = term.filter { $0.isLetter || $0.isNumber }
        guard alphanumerics.contains(where: \.isLetter) else { return false }
        // Avoid learning token-like secrets and hashes.
        if term.count > 24, !term.contains(" "),
           term.filter(\.isNumber).count > 4 { return false }
        return true
    }
}

/// Conservative formatter/corrector. It changes only spans whose compacted
/// spelling is identical or nearly identical to a visible/learned term.
public enum TerminologyCorrector {
    private static let wordRegex = try! NSRegularExpression(
        pattern: #"[\p{L}\p{N}]+(?:['’][\p{L}\p{N}]+)?"#)

    public static func correct(
        _ text: String,
        screenTerms: [String],
        learnedTerms: [LearnedTerm],
        protectedTerms: [String] = []
    ) -> TerminologyCorrectionResult {
        let ns = text as NSString
        let words = wordRegex.matches(
            in: text, range: NSRange(location: 0, length: ns.length))
        guard !words.isEmpty else { return .init(text: text, matches: []) }
        let protected = Set(protectedTerms.map(ScreenTermExtractor.normalized))

        struct Candidate { let canonical: String; let aliases: [String]; let source: TerminologyMatch.Source }
        var candidates = learnedTerms.map {
            Candidate(canonical: $0.canonical, aliases: [$0.canonical] + $0.aliases, source: .learned)
        }
        candidates += screenTerms.map {
            Candidate(canonical: $0, aliases: [$0], source: .screen)
        }

        struct Replacement {
            let range: NSRange
            let canonical: String
            let heard: String
            let confidence: Double
            let source: TerminologyMatch.Source
        }
        var proposals: [Replacement] = []
        for candidate in candidates {
            let canonicalKey = ScreenTermExtractor.normalized(candidate.canonical)
            guard canonicalKey.count >= 2, !protected.contains(canonicalKey) else { continue }
            let comparisonAliases = candidate.aliases.flatMap { [$0, spokenForm($0)] }
            let canonicalWordCount = comparisonAliases.map {
                $0.split(whereSeparator: \.isWhitespace).count
            }.max() ?? 1
            let minWords = max(1, canonicalWordCount - 1)
            let maxWords = min(8, canonicalWordCount + 2)
            for start in words.indices {
                for count in minWords...maxWords where start + count <= words.count {
                    let first = words[start].range
                    let last = words[start + count - 1].range
                    let range = NSRange(
                        location: first.location,
                        length: last.location + last.length - first.location)
                    let heard = ns.substring(with: range)
                    let heardKey = ScreenTermExtractor.normalized(heard)
                    guard !heardKey.isEmpty,
                          !protected.contains(heardKey),
                          heard != candidate.canonical else { continue }
                    let heardWords = heard.lowercased().split(whereSeparator: \.isWhitespace)
                    if !candidate.canonical.contains("/"),
                       (heardWords.contains("slash") || heardWords.contains("backslash")
                        || (start > words.startIndex && ["slash", "backslash"].contains(
                            ns.substring(with: words[start - 1].range).lowercased()))) {
                        // This is a filename component inside a spoken path;
                        // wait for the full path candidate instead of consuming
                        // the high-confidence suffix on its own.
                        continue
                    }

                    var confidence = 0.0
                    for alias in comparisonAliases {
                        let aliasKey = ScreenTermExtractor.normalized(alias)
                        if heardKey == aliasKey { confidence = 1; break }
                        guard min(heardKey.count, aliasKey.count) >= 6 else { continue }
                        confidence = max(confidence, similarity(heardKey, aliasKey))
                    }
                    let structured = isStructuredSpokenMatch(
                        canonical: candidate.canonical, heard: heard)
                    let threshold = structured ? 0.72
                        : (candidate.source == .screen ? 0.88 : 0.84)
                    if confidence >= threshold {
                        proposals.append(.init(
                            range: range, canonical: candidate.canonical, heard: heard,
                            confidence: confidence, source: candidate.source))
                    }
                }
            }
        }

        proposals.sort {
            if $0.confidence != $1.confidence { return $0.confidence > $1.confidence }
            if $0.range.length != $1.range.length { return $0.range.length > $1.range.length }
            return $0.source == .learned && $1.source == .screen
        }
        var accepted: [Replacement] = []
        for proposal in proposals where !accepted.contains(where: { NSIntersectionRange($0.range, proposal.range).length > 0 }) {
            accepted.append(proposal)
        }
        accepted.sort { $0.range.location < $1.range.location }

        let output = NSMutableString(string: text)
        for replacement in accepted.reversed() {
            output.replaceCharacters(in: replacement.range, with: replacement.canonical)
        }
        return .init(
            text: output as String,
            matches: accepted.map {
                .init(heard: $0.heard, canonical: $0.canonical,
                      confidence: $0.confidence, source: $0.source)
            })
    }

    private static func similarity(_ lhs: String, _ rhs: String) -> Double {
        let a = Array(lhs), b = Array(rhs)
        guard !a.isEmpty, !b.isEmpty else { return 0 }
        var previous = Array(0...b.count)
        for (i, x) in a.enumerated() {
            var current = [i + 1] + Array(repeating: 0, count: b.count)
            for (j, y) in b.enumerated() {
                current[j + 1] = min(
                    current[j] + 1,
                    previous[j + 1] + 1,
                    previous[j] + (x == y ? 0 : 1))
            }
            previous = current
        }
        return 1 - Double(previous[b.count]) / Double(max(a.count, b.count))
    }

    private static func spokenForm(_ canonical: String) -> String {
        canonical
            .replacingOccurrences(of: "/", with: " slash ")
            .replacingOccurrences(of: "\\", with: " backslash ")
            .replacingOccurrences(of: ".", with: " dot ")
            .replacingOccurrences(of: "_", with: " underscore ")
    }

    private static func isStructuredSpokenMatch(canonical: String, heard: String) -> Bool {
        guard canonical.contains(where: { "/\\._".contains($0) }) else { return false }
        let lowerHeard = heard.lowercased()
        guard ["slash", "backslash", "dot", "underscore"].contains(where: lowerHeard.contains)
        else { return false }
        // Require the final path component to be clearly present. This allows
        // a mangled parent directory while preventing loose path-shaped text
        // from matching an unrelated visible file.
        let finalComponent = canonical.split(whereSeparator: { $0 == "/" || $0 == "\\" }).last
            .map(String.init) ?? canonical
        let finalSpokenKey = ScreenTermExtractor.normalized(spokenForm(finalComponent))
        return lowerHeard.split(whereSeparator: \.isWhitespace).indices.contains { start in
            let suffix = lowerHeard.split(whereSeparator: \.isWhitespace)[start...].joined()
            return ScreenTermExtractor.normalized(suffix).contains(finalSpokenKey)
        }
    }
}
