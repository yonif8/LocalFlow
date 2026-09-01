import Foundation
import Testing
@testable import LFPolish

@Suite("Replacement engine")
struct ReplacementEngineTests {
    private func engine(
        _ pairs: [(String, String)],
        punctuation: Bool = false
    ) -> ReplacementEngine {
        ReplacementEngine(dictionary: PersonalDictionary(
            rules: pairs.map { ReplacementRule(spoken: $0.0, written: $0.1) },
            spokenPunctuationEnabled: punctuation))
    }

    @Test func basicReplacement() {
        let e = engine([("echidna cams", "EchidnaCams")])
        #expect(e.apply(to: "check out echidna cams today") == "check out EchidnaCams today")
    }

    @Test func caseInsensitiveMatching() {
        let e = engine([("echidna cams", "EchidnaCams")])
        #expect(e.apply(to: "Echidna Cams is live") == "EchidnaCams is live")
        #expect(e.apply(to: "ECHIDNA CAMS is live") == "EchidnaCams is live")
    }

    @Test func wholeWordBoundaries() {
        let e = engine([("cat", "CAT")])
        #expect(e.apply(to: "the cat sat") == "the CAT sat")
        // No substring matches inside larger words.
        #expect(e.apply(to: "concatenate the category") == "concatenate the category")
        // Punctuation adjacency still counts as a boundary.
        #expect(e.apply(to: "my cat, obviously") == "my CAT, obviously")
    }

    @Test func casePreservingApplyForLowercaseWrittenForms() {
        let e = engine([("localflow", "localflow app")])
        // Sentence-start capitalization is mirrored.
        #expect(e.apply(to: "Localflow is great") == "Localflow app is great")
        // ALL CAPS is mirrored.
        #expect(e.apply(to: "LOCALFLOW is great") == "LOCALFLOW APP is great")
        // Lowercase stays lowercase.
        #expect(e.apply(to: "use localflow daily") == "use localflow app daily")
    }

    @Test func authorCasedWrittenFormIsVerbatim() {
        let e = engine([("i b m", "IBM"), ("echidna cams", "EchidnaCams")])
        // Written forms containing uppercase are used exactly as authored,
        // regardless of how the match was cased.
        #expect(e.apply(to: "I B M mainframes") == "IBM mainframes")
        #expect(e.apply(to: "ECHIDNA CAMS rules") == "EchidnaCams rules")
    }

    @Test func longestMatchWins() {
        let e = engine([("new york", "New York"), ("new york times", "The New York Times")])
        #expect(e.apply(to: "read the new york times daily") == "read the The New York Times daily")
        #expect(e.apply(to: "i love new york a lot") == "i love New York a lot")
    }

    @Test func flexibleWhitespaceBetweenSpokenWords() {
        let e = engine([("echidna cams", "EchidnaCams")])
        #expect(e.apply(to: "echidna  cams") == "EchidnaCams")
    }

    @Test func multipleOccurrencesAndRules() {
        let e = engine([("foo bar", "FooBar"), ("baz", "BAZ")])
        #expect(e.apply(to: "foo bar and baz and foo bar") == "FooBar and BAZ and FooBar")
    }

    @Test func emptyDictionaryIsNoOp() {
        let e = engine([])
        let text = "um, hello there echidna cams"
        #expect(e.apply(to: text) == text)
    }

    @Test func emptyInput() {
        let e = engine([("a", "b")])
        #expect(e.apply(to: "") == "")
    }

    @Test func idempotency() {
        let e = engine([
            ("echidna cams", "EchidnaCams"),
            ("i b m", "IBM"),
            ("localflow", "LocalFlow"),
        ])
        let input = "Localflow syncs echidna cams footage to i b m cloud"
        let once = e.apply(to: input)
        #expect(e.apply(to: once) == once)
    }

    @Test func replacementOutputIsNotRematchedInSamePass() {
        // "alpha" -> "beta" and "beta" -> "gamma": single-pass semantics mean
        // alpha becomes beta and existing beta becomes gamma, no cascading.
        let e = engine([("alpha", "beta"), ("beta", "gamma")])
        #expect(e.apply(to: "alpha beta") == "beta gamma")
    }

    @Test func spokenPunctuationOffByDefault() {
        #expect(PersonalDictionary().spokenPunctuationEnabled == false)
        let e = engine([("echidna cams", "EchidnaCams")])
        #expect(e.apply(to: "hello comma world period") == "hello comma world period")
    }

    @Test func spokenPunctuationWhenEnabled() {
        let e = engine([], punctuation: true)
        #expect(e.apply(to: "hello comma world period") == "hello, world.")
        #expect(e.apply(to: "really question mark") == "really?")
        #expect(e.apply(to: "first line new line second line") == "first line\nsecond line")
        #expect(e.apply(to: "one new paragraph two") == "one\n\ntwo")
    }

    @Test func userRulesRunBeforePunctuation() {
        let e = engine([("echidna cams", "EchidnaCams")], punctuation: true)
        #expect(e.apply(to: "echidna cams is live period") == "EchidnaCams is live.")
    }
}

@Suite("Personal dictionary persistence")
struct PersistenceTests {
    @Test func jsonRoundTrip() throws {
        let dictionary = PersonalDictionary(
            rules: [
                ReplacementRule(spoken: "echidna cams", written: "EchidnaCams"),
                ReplacementRule(spoken: "i b m", written: "IBM"),
            ],
            spokenPunctuationEnabled: true)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("lfpolish-test-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }

        try dictionary.save(to: url)
        let loaded = try PersonalDictionary.load(from: url)
        #expect(loaded == dictionary)
    }

    @Test func loadMissingFileThrows() {
        let url = URL(fileURLWithPath: "/nonexistent/lfpolish-\(UUID().uuidString).json")
        #expect(throws: (any Error).self) {
            _ = try PersonalDictionary.load(from: url)
        }
    }
}

@Suite("Output plausibility guardrail")
struct PlausibilityTests {
    @Test func acceptsRealCleanup() {
        #expect(LocalPolisher.looksLikeCleanup(
            of: "um so hey can you uh send me the report by friday",
            candidate: "Hey, can you send me the report by Friday?"))
    }

    // Note: a short ANSWER that reuses the dictation's own words ("Sure, I
    // can send you the report.") passes the generic overlap check — that
    // failure class is prevented at the source by using a non-chat
    // normalizer model (S1-mini), not by output filtering.

    @Test func rejectsCannedAssistantFiller() {
        #expect(!LocalPolisher.looksLikeCleanup(
            of: "check the logs and tell me what is slow about the pipeline today",
            candidate: "I apologize for the delay. I'm currently processing your request. It may take a few seconds to complete."))
    }

    @Test func rejectsGuttedOrBloatedOutput() {
        #expect(!LocalPolisher.looksLikeCleanup(
            of: "this is a reasonably long dictated sentence with plenty of words in it",
            candidate: "ok"))
    }
}
