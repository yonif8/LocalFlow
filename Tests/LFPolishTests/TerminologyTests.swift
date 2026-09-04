import Foundation
import Testing
@testable import LFPolish

@Suite("Screen terminology extraction")
struct ScreenTermExtractorTests {
    @Test func excludesGenericChatLabel() {
        #expect(!ScreenTermExtractor.extract(from: ["Chat"]).contains("Chat"))
    }

    @Test func extractsGenericTechnicalShapesAndNames() {
        let terms = ScreenTermExtractor.extract(from: [
            "Configure PostgreSQL in DataGrip",
            "Connect Jane Smith to API v2",
            "Open LocalFlow/Sources/auth_token.swift",
        ])
        #expect(terms.contains("PostgreSQL"))
        #expect(terms.contains("DataGrip"))
        #expect(terms.contains("Jane Smith"))
        #expect(terms.contains("API"))
        #expect(terms.contains("v2"))
    }

    @Test func extractsUsefulSuffixesFromAbsolutePaths() {
        let terms = ScreenTermExtractor.extract(from: [
            "/Users/yoni/Projects/LocalFlow/Sources/LFPolish/Terminology.swift"
        ])
        #expect(terms.contains("LFPolish/Terminology.swift"))
        #expect(terms.contains("Terminology.swift"))
    }

    @Test func extractsNamesWithLowercaseConnectors() {
        let terms = ScreenTermExtractor.extract(from: ["Bab el-Mandeb Ship Count"])
        #expect(terms.contains("Bab el-Mandeb"))
    }

    @Test func onlyStrongTechnicalTermsArePersistent() {
        #expect(!ScreenTermExtractor.isPersistentCandidate("The"))
        #expect(!ScreenTermExtractor.isPersistentCandidate("Check"))
        #expect(ScreenTermExtractor.isPersistentCandidate("PostgreSQL"))
        #expect(ScreenTermExtractor.isPersistentCandidate("Terminology.swift"))
        #expect(ScreenTermExtractor.isPersistentCandidate("Jane Smith"))
    }

    @Test func excludesLikelySecretsAndAddresses() {
        let terms = ScreenTermExtractor.extract(from: [
            "person@example.com",
            "A94F20B81C7348899918273619283746",
            "https://internal.example.com/SecretProject",
            "Settings Save Cancel",
        ])
        #expect(!terms.contains(where: { $0.contains("@") }))
        #expect(!terms.contains(where: { $0.localizedCaseInsensitiveContains("SecretProject") }))
        #expect(!terms.contains("A94F20B81C7348899918273619283746"))
        #expect(!terms.contains("Settings"))
    }
}

@Suite("Terminology correction")
struct TerminologyCorrectorTests {
    @Test func restoresVisibleCasingAndWordBoundaries() {
        let result = TerminologyCorrector.correct(
            "configure postgresql in data grip",
            screenTerms: ["PostgreSQL", "DataGrip"], learnedTerms: [])
        #expect(result.text == "configure PostgreSQL in DataGrip")
        #expect(result.matches.count == 2)
        #expect(result.matches.allSatisfy { $0.source == .screen })
    }

    @Test func allowsOnlyVeryCloseScreenTypos() {
        let corrected = TerminologyCorrector.correct(
            "open postgressql", screenTerms: ["PostgreSQL"], learnedTerms: [])
        #expect(corrected.text == "open PostgreSQL")

        let untouched = TerminologyCorrector.correct(
            "open postal sequel", screenTerms: ["PostgreSQL"], learnedTerms: [])
        #expect(untouched.text == "open postal sequel")
    }

    @Test func learnedAliasWorksAwayFromSourceWindow() {
        let learned = LearnedTerm(
            canonical: "PostgreSQL", aliases: ["postgressql"], useCount: 3)
        let result = TerminologyCorrector.correct(
            "use postgressql tomorrow", screenTerms: [], learnedTerms: [learned])
        #expect(result.text == "use PostgreSQL tomorrow")
        #expect(result.matches.first?.source == .learned)
    }

    @Test func personalDictionarySpellingsAreProtected() {
        let result = TerminologyCorrector.correct(
            "use Postgres tomorrow", screenTerms: ["PostgreSQL"], learnedTerms: [],
            protectedTerms: ["Postgres"])
        #expect(result.text == "use Postgres tomorrow")
        #expect(result.matches.isEmpty)
    }

    @Test func ordinaryUnrelatedWordsAreUntouched() {
        let result = TerminologyCorrector.correct(
            "please send the report tomorrow",
            screenTerms: ["Repository", "Tomorrowland"], learnedTerms: [])
        #expect(result.text == "please send the report tomorrow")
    }

    @Test func screenDoesNotCapitalizeOrdinarySingleWords() {
        let result = TerminologyCorrector.correct(
            "Can you decide what to enable and what to disable?",
            screenTerms: ["You", "And", "Check"], learnedTerms: [])
        #expect(result.text == "Can you decide what to enable and what to disable?")
        #expect(result.matches.isEmpty)
    }

    @Test func explicitlyLearnedSingleWordCanRestoreCapitalization() {
        let result = TerminologyCorrector.correct(
            "ask alice tomorrow", screenTerms: [],
            learnedTerms: [.init(canonical: "Alice")])
        #expect(result.text == "ask Alice tomorrow")
        #expect(result.matches.first?.source == .learned)
    }


    @Test func recoversSpokenFilePathWhenDirectoryIsMangledByASR() {
        let result = TerminologyCorrector.correct(
            "L'Philippines slash terminology dot swift.",
            screenTerms: ["LFPolish/Terminology.swift", "Terminology.swift"],
            learnedTerms: [])
        #expect(result.text == "LFPolish/Terminology.swift.")
        #expect(result.matches.count == 1)
        #expect(result.matches.first?.canonical == "LFPolish/Terminology.swift")
    }


    @Test func restoresSeparatorsWhenPathComponentsMatchIndividually() {
        let result = TerminologyCorrector.correct(
            "sources slash local flow app slash bug report view dot swift.",
            screenTerms: ["LocalFlowApp", "BugReportView.swift"],
            learnedTerms: [])
        #expect(result.text == "sources/LocalFlowApp/BugReportView.swift.")
    }

    @Test func correctsShortNameInsideAnchoredProperPhrase() {
        let result = TerminologyCorrector.correct(
            "Bob Elman Deb ship count",
            screenTerms: ["Bab el-Mandeb"], learnedTerms: [])
        #expect(result.text == "Bab el-Mandeb ship count")
    }

    @Test func visibleTermWinsOverConflictingMemory() {
        let result = TerminologyCorrector.correct(
            "deep seek harness",
            screenTerms: ["DeepSeek Harness"],
            learnedTerms: [.init(canonical: "DeepSea", aliases: ["deep sea"])])
        #expect(result.text == "DeepSeek Harness")
        #expect(result.matches.first?.source == .screen)
    }
}
