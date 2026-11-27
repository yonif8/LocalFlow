import Testing
@testable import LFInsert

@Suite("WhatsApp send policy")
struct WhatsAppSenderTests {
    @Test("normalizes international numbers to digits")
    func normalizesNumbers() {
        #expect(WhatsAppRecipient.normalize("+61 412 345 678") == "61412345678")
        #expect(WhatsAppRecipient.normalize("+1 (415) 555-0100") == "14155550100")
        #expect(WhatsAppRecipient.normalize("0061412345678") == "61412345678")
        #expect(WhatsAppRecipient.normalize(" 972526389367 ") == "972526389367")
    }

    @Test("rejects names, local-format and implausible numbers")
    func rejectsInvalid() {
        #expect(WhatsAppRecipient.normalize("") == nil)
        #expect(WhatsAppRecipient.normalize("Mum") == nil)
        #expect(WhatsAppRecipient.normalize("0412 345 678") == nil)   // no country code
        #expect(WhatsAppRecipient.normalize("+12345") == nil)         // too short
        #expect(WhatsAppRecipient.normalize("+1234567890123456") == nil) // > 15 digits
        #expect(WhatsAppRecipient.normalize("+61 412&345") == nil)
    }

    @Test("text inputs count as editable even when AX says unsettable")
    func editableFocus() {
        #expect(EditableFocusPolicy.isEditable(role: "AXWebArea", subrole: nil, selectedTextSettable: true))
        #expect(EditableFocusPolicy.isEditable(role: "AXTextArea", subrole: nil, selectedTextSettable: false))
        #expect(EditableFocusPolicy.isEditable(role: "AXTextField", subrole: "AXSecureTextField", selectedTextSettable: false))
        #expect(!EditableFocusPolicy.isEditable(role: nil, subrole: nil, selectedTextSettable: false))
        #expect(!EditableFocusPolicy.isEditable(role: "AXWebArea", subrole: nil, selectedTextSettable: false))
        #expect(!EditableFocusPolicy.isEditable(role: "AXButton", subrole: nil, selectedTextSettable: false))
    }

    @Test("reads WhatsApp's staged media count")
    func mediaCount() {
        #expect(WhatsAppMediaCount.parse("\u{200E}1 media item.") == 1)
        #expect(WhatsAppMediaCount.parse("175 media items.") == 175)
        #expect(WhatsAppMediaCount.parse("media items") == nil)
    }

    @Test("caption is trimmed and capped at WhatsApp's 1024 characters")
    @MainActor
    func captionCap() {
        #expect(WhatsAppSender.trimmedCaption("  hi \n") == "hi")
        let long = String(repeating: "a", count: 2000)
        #expect(WhatsAppSender.trimmedCaption(long).count == 1024)
    }
}
