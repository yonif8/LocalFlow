import AppKit
import Foundation
import Testing
@testable import LFInsert

/// Tests run against uniquely named pasteboards so they never touch the user's
/// general pasteboard and can run in parallel.
private func makeScratchPasteboard() -> NSPasteboard {
    NSPasteboard(name: NSPasteboard.Name("com.localflow.lfinsert.tests.\(UUID().uuidString)"))
}

@Suite("PasteboardTransfer")
struct PasteboardTransferTests {
    @Test("snapshot and restore round-trips multiple items and types")
    func roundTrip() {
        let pb = makeScratchPasteboard()
        defer { pb.releaseGlobally() }

        let customType = NSPasteboard.PasteboardType("com.localflow.tests.custom")
        pb.clearContents()
        let itemA = NSPasteboardItem()
        itemA.setString("hello original", forType: .string)
        itemA.setData(Data([0xDE, 0xAD, 0xBE, 0xEF]), forType: customType)
        let itemB = NSPasteboardItem()
        itemB.setString("<b>rich</b>", forType: .html)
        pb.writeObjects([itemA, itemB])

        let snapshot = PasteboardTransfer.snapshot(of: pb)
        #expect(snapshot.items.count == 2)

        // Clobber, as a paste insertion would.
        let ourCount = PasteboardTransfer.setTransientString("dictated text", on: pb)
        #expect(pb.string(forType: .string) == "dictated text")

        let restored = PasteboardTransfer.restore(
            snapshot, to: pb, ifChangeCountStillEquals: ourCount
        )
        #expect(restored)

        let items = pb.pasteboardItems ?? []
        #expect(items.count == 2)
        #expect(items.first?.string(forType: .string) == "hello original")
        #expect(items.first?.data(forType: customType) == Data([0xDE, 0xAD, 0xBE, 0xEF]))
        #expect(items.last?.string(forType: .html) == "<b>rich</b>")
        // The transient marker must be gone after restore.
        #expect(pb.data(forType: PasteboardTransfer.transientType) == nil)
    }

    @Test("transient write carries the org.nspasteboard.TransientType marker")
    func transientMarker() {
        let pb = makeScratchPasteboard()
        defer { pb.releaseGlobally() }

        PasteboardTransfer.setTransientString("ephemeral", on: pb)
        #expect(pb.string(forType: .string) == "ephemeral")
        #expect(pb.data(forType: PasteboardTransfer.transientType) != nil)
        #expect(
            pb.pasteboardItems?.first?.types.contains(PasteboardTransfer.transientType) == true
        )
    }

    @Test("restore is skipped when someone else changed the pasteboard")
    func restoreSkippedOnRace() {
        let pb = makeScratchPasteboard()
        defer { pb.releaseGlobally() }

        pb.clearContents()
        pb.setString("original", forType: .string)
        let snapshot = PasteboardTransfer.snapshot(of: pb)

        let ourCount = PasteboardTransfer.setTransientString("dictated", on: pb)

        // A third party (e.g. the user copying) writes during the paste delay.
        pb.clearContents()
        pb.setString("user copied this meanwhile", forType: .string)
        #expect(pb.changeCount != ourCount)

        let restored = PasteboardTransfer.restore(
            snapshot, to: pb, ifChangeCountStillEquals: ourCount
        )
        #expect(!restored)
        #expect(pb.string(forType: .string) == "user copied this meanwhile")
    }

    @Test("restoring an empty snapshot clears the pasteboard")
    func emptySnapshot() {
        let pb = makeScratchPasteboard()
        defer { pb.releaseGlobally() }

        pb.clearContents()
        let snapshot = PasteboardTransfer.snapshot(of: pb)
        #expect(snapshot.isEmpty)

        let ourCount = PasteboardTransfer.setTransientString("dictated", on: pb)
        let restored = PasteboardTransfer.restore(
            snapshot, to: pb, ifChangeCountStillEquals: ourCount
        )
        #expect(restored)
        #expect(pb.string(forType: .string) == nil)
        #expect((pb.pasteboardItems ?? []).isEmpty)
    }

    @Test("unconditional restore ignores change count")
    func unconditionalRestore() {
        let pb = makeScratchPasteboard()
        defer { pb.releaseGlobally() }

        pb.clearContents()
        pb.setString("original", forType: .string)
        let snapshot = PasteboardTransfer.snapshot(of: pb)

        pb.clearContents()
        pb.setString("something else", forType: .string)

        let restored = PasteboardTransfer.restore(snapshot, to: pb)
        #expect(restored)
        #expect(pb.string(forType: .string) == "original")
    }
}

@Suite("InserterConfiguration")
struct InserterConfigurationTests {
    @Test("default strategy chain is AX then paste, with typing disabled")
    func defaultChain() {
        let config = InserterConfiguration.default
        #expect(config.strategyOrder == [.ax, .paste])
        #expect(!config.strategyOrder.contains(.type))
    }

    @Test("strategy order is configurable")
    func customChain() {
        let config = InserterConfiguration(strategyOrder: [.paste, .ax, .type])
        #expect(config.strategyOrder == [.paste, .ax, .type])
    }

    @Test("chunk size is clamped to at least 1")
    func chunkClamp() {
        #expect(InserterConfiguration(typingChunkSize: 0).typingChunkSize == 1)
    }
}
