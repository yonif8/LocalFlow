import AppKit

/// A deep copy of a pasteboard's contents: every item with every type whose
/// data could be materialized. Value-typed and Sendable so it can be held
/// across the paste delay.
public struct PasteboardSnapshot: Sendable {
    public struct TypedData: Sendable {
        public let type: String
        public let data: Data

        public init(type: String, data: Data) {
            self.type = type
            self.data = data
        }
    }

    public struct Item: Sendable {
        public let representations: [TypedData]

        public init(representations: [TypedData]) {
            self.representations = representations
        }
    }

    public let items: [Item]
    /// The pasteboard's changeCount at snapshot time.
    public let changeCount: Int

    public init(items: [Item], changeCount: Int) {
        self.items = items
        self.changeCount = changeCount
    }

    public var isEmpty: Bool { items.allSatisfy { $0.representations.isEmpty } }
}

/// Save/replace/restore helpers around a paste-based insertion.
public enum PasteboardTransfer {
    /// Marker type telling well-behaved clipboard managers to ignore this write.
    /// See http://nspasteboard.org
    public static let transientType = NSPasteboard.PasteboardType("org.nspasteboard.TransientType")

    /// Deep-copies everything on `pasteboard` that can be round-tripped
    /// (lazy/promised data is materialized by `data(forType:)`; types that
    /// return nil are skipped).
    public static func snapshot(of pasteboard: NSPasteboard) -> PasteboardSnapshot {
        let items = (pasteboard.pasteboardItems ?? []).map { item in
            PasteboardSnapshot.Item(representations: item.types.compactMap { type in
                item.data(forType: type).map { PasteboardSnapshot.TypedData(type: type.rawValue, data: $0) }
            })
        }
        return PasteboardSnapshot(items: items, changeCount: pasteboard.changeCount)
    }

    /// Replaces the pasteboard contents with `text`, marked with the
    /// `org.nspasteboard.TransientType` type so clipboard managers ignore it.
    /// - Returns: the pasteboard's changeCount after the write, for use with
    ///   `restore(_:to:ifChangeCountStillEquals:)`.
    @discardableResult
    public static func setTransientString(_ text: String, on pasteboard: NSPasteboard) -> Int {
        pasteboard.clearContents()
        let item = NSPasteboardItem()
        item.setString(text, forType: .string)
        item.setData(Data(), forType: transientType)
        pasteboard.writeObjects([item])
        return pasteboard.changeCount
    }

    /// Restores a snapshot. If `expectedChangeCount` is given and the
    /// pasteboard has since been changed by someone else (e.g. the user copied
    /// something during the paste delay), the restore is skipped.
    /// - Returns: true if the snapshot was written back, false if skipped.
    @discardableResult
    public static func restore(
        _ snapshot: PasteboardSnapshot,
        to pasteboard: NSPasteboard,
        ifChangeCountStillEquals expectedChangeCount: Int? = nil
    ) -> Bool {
        if let expected = expectedChangeCount, pasteboard.changeCount != expected {
            return false
        }
        pasteboard.clearContents()
        guard !snapshot.items.isEmpty else { return true }
        let items = snapshot.items.map { saved -> NSPasteboardItem in
            let item = NSPasteboardItem()
            for rep in saved.representations {
                item.setData(rep.data, forType: NSPasteboard.PasteboardType(rep.type))
            }
            return item
        }
        pasteboard.writeObjects(items)
        return true
    }
}
