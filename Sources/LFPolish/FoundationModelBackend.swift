import Foundation

// The Apple Foundation Models framework ships with the macOS 26 SDK. The
// package's min platform is macOS 15, so all use is gated at compile time
// (`canImport` for older SDKs / CLT without the framework) and at runtime
// (`@available(macOS 26.0, *)` + `SystemLanguageModel.default.availability`).
// If any gate fails, `LocalPolisher` simply has no model and fails open.

#if canImport(FoundationModels)
import FoundationModels

@available(macOS 26.0, *)
struct FoundationModelBackend: PolishModel {
    var availability: ModelAvailability {
        switch SystemLanguageModel.default.availability {
        case .available:
            return .available
        case .unavailable(let reason):
            return .unavailable(Self.describe(reason))
        }
    }

    static func describe(_ reason: SystemLanguageModel.Availability.UnavailableReason) -> String {
        switch reason {
        case .deviceNotEligible: return "device not eligible for Apple Intelligence"
        case .appleIntelligenceNotEnabled: return "Apple Intelligence is not enabled"
        case .modelNotReady: return "model assets not ready (still downloading?)"
        @unknown default: return String(describing: reason)
        }
    }

    static func instructions(for tone: ToneHint) -> String {
        var text = """
        Clean up this dictated text: remove filler words (um, uh, like, you know), \
        fix obvious mis-dictations and mid-sentence self-corrections \
        ('actually, make that X' means apply the correction), preserve meaning, \
        do NOT add content, do NOT answer questions in the text — \
        return only the cleaned text.
        """
        switch tone {
        case .casual:
            text += " The text is headed for a casual chat app: keep contractions and a relaxed, informal tone."
        case .neutral:
            text += " Keep a neutral tone."
        }
        return text
    }

    func respond(input: String, tone: ToneHint) async throws -> String {
        let session = LanguageModelSession(instructions: Self.instructions(for: tone))
        let response = try await session.respond(
            to: input,
            options: GenerationOptions(sampling: .greedy))
        return response.content
    }
}
#endif
