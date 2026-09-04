import AppKit
import ApplicationServices
import Foundation
import LFPolish
import os

struct ScreenContextSnapshot: Sendable {
    let terms: [String]
    let bundleID: String?
    let visitedElements: Int
    let elapsed: Duration
}

/// Reads a small, bounded portion of the active window's accessibility tree.
/// Secure fields are skipped and collected strings never leave the process.
enum ScreenContextCollector {
    static func capture() -> ScreenContextSnapshot {
        let clock = ContinuousClock()
        let start = clock.now
        guard AXIsProcessTrusted(),
              let app = NSWorkspace.shared.frontmostApplication,
              app.bundleIdentifier != Bundle.main.bundleIdentifier else {
            return .init(terms: [], bundleID: nil, visitedElements: 0, elapsed: clock.now - start)
        }

        let appElement = AXUIElementCreateApplication(app.processIdentifier)
        guard let window = elementAttribute(appElement, kAXFocusedWindowAttribute as String)
                ?? elementAttribute(appElement, kAXMainWindowAttribute as String) else {
            return .init(terms: [], bundleID: app.bundleIdentifier,
                         visitedElements: 0, elapsed: clock.now - start)
        }

        let deadline = start.advanced(by: .milliseconds(45))
        var queue = [window]
        var cursor = 0
        var strings: [String] = []
        var totalCharacters = 0
        while cursor < queue.count, cursor < 700, clock.now < deadline,
              totalCharacters < 20_000 {
            let element = queue[cursor]
            cursor += 1
            let role = stringAttribute(element, kAXRoleAttribute as String)
            let subrole = stringAttribute(element, kAXSubroleAttribute as String)
            if role == "AXSecureTextField" || subrole == "AXSecureTextField" { continue }
            if boolAttribute(element, kAXHiddenAttribute as String) == true { continue }

            for attribute in [kAXTitleAttribute, kAXDescriptionAttribute,
                              kAXHelpAttribute, kAXValueAttribute] {
                guard let value = stringAttribute(element, attribute as String),
                      !value.isEmpty, value.count <= 500 else { continue }
                strings.append(value)
                totalCharacters += value.count
            }
            queue.append(contentsOf: elementsAttribute(element, kAXVisibleChildrenAttribute as String)
                ?? elementsAttribute(element, kAXChildrenAttribute as String) ?? [])
        }
        return .init(
            terms: ScreenTermExtractor.extract(from: strings),
            bundleID: app.bundleIdentifier,
            visitedElements: cursor,
            elapsed: clock.now - start)
    }

    private static func attribute(_ element: AXUIElement, _ name: String) -> CFTypeRef? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else {
            return nil
        }
        return value
    }

    private static func stringAttribute(_ element: AXUIElement, _ name: String) -> String? {
        attribute(element, name) as? String
    }

    private static func boolAttribute(_ element: AXUIElement, _ name: String) -> Bool? {
        attribute(element, name) as? Bool
    }

    private static func elementAttribute(_ element: AXUIElement, _ name: String) -> AXUIElement? {
        guard let value = attribute(element, name),
              CFGetTypeID(value) == AXUIElementGetTypeID() else { return nil }
        return (value as! AXUIElement)
    }

    private static func elementsAttribute(_ element: AXUIElement, _ name: String) -> [AXUIElement]? {
        attribute(element, name) as? [AXUIElement]
    }
}

enum LearnedTerminologyStore {
    private static let logger = Logger(subsystem: "com.localflow.context", category: "learning")

    static func load() -> [LearnedTerm] {
        guard let data = try? Data(contentsOf: AppSettings.learnedTerminologyURL) else { return [] }
        do {
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601
            return try decoder.decode([LearnedTerm].self, from: data)
        }
        catch {
            logger.error("could not load learned terminology: \(String(describing: error), privacy: .public)")
            return []
        }
    }

    static func learn(_ matches: [TerminologyMatch], sourceBundleID: String?) {
        let screenMatches = matches.filter { $0.source == .screen && $0.confidence >= 0.88 }
        guard !screenMatches.isEmpty else { return }
        var terms = load()
        let now = Date()
        for match in screenMatches {
            let key = ScreenTermExtractor.normalized(match.canonical)
            if let index = terms.firstIndex(where: {
                ScreenTermExtractor.normalized($0.canonical) == key
            }) {
                if match.heard.caseInsensitiveCompare(match.canonical) != .orderedSame,
                   !terms[index].aliases.contains(where: {
                       $0.caseInsensitiveCompare(match.heard) == .orderedSame
                   }) {
                    terms[index].aliases.append(match.heard)
                }
                terms[index].useCount += 1
                terms[index].lastUsedAt = now
            } else {
                let aliases = match.heard.caseInsensitiveCompare(match.canonical) == .orderedSame
                    ? [] : [match.heard]
                terms.append(.init(canonical: match.canonical, aliases: aliases,
                                   sourceBundleID: sourceBundleID))
            }
        }
        terms.sort { $0.lastUsedAt > $1.lastUsedAt }
        save(Array(terms.prefix(500)))
    }

    static func save(_ terms: [LearnedTerm]) {
        do {
            let directory = AppSettings.learnedTerminologyURL.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            encoder.dateEncodingStrategy = .iso8601
            // Decode both ISO-8601 and legacy default isn't needed before first release.
            try encoder.encode(terms).write(to: AppSettings.learnedTerminologyURL, options: .atomic)
        } catch {
            logger.error("could not save learned terminology: \(String(describing: error), privacy: .public)")
        }
    }

    static func removeAll() { save([]) }
}
