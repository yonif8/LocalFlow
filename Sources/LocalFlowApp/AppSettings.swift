import Foundation
import LFInsert
import LFPolish

/// Typed access to every user-tunable parameter, with the app's defaults.
/// SettingsView writes these; DictationCoordinator.applySettings() re-reads
/// them and rebuilds the affected pipeline pieces.
enum AppSettings {
    private static var defaults: UserDefaults { .standard }

    // MARK: General

    static var hudEnabled: Bool {
        get { defaults.object(forKey: DefaultsKey.hudEnabled) as? Bool ?? true }
        set { defaults.set(newValue, forKey: DefaultsKey.hudEnabled) }
    }

    static var historyLimit: Int {
        get { clamp(defaults.object(forKey: DefaultsKey.historyLimit) as? Int ?? 10, 0, 50) }
        set { defaults.set(newValue, forKey: DefaultsKey.historyLimit) }
    }

    // MARK: Dictation / capture

    static var keepMicWarm: Bool {
        get { defaults.object(forKey: DefaultsKey.keepMicWarm) as? Bool ?? false }
        set { defaults.set(newValue, forKey: DefaultsKey.keepMicWarm) }
    }

    /// Duck the system's audio output to 20% while push-to-talk records, so
    /// music doesn't drown out the microphone. Read at event time by
    /// DictationCoordinator; no pipeline rebuild needed.
    static var duckWhileDictating: Bool {
        get { defaults.object(forKey: DefaultsKey.duckWhileDictating) as? Bool ?? true }
        set { defaults.set(newValue, forKey: DefaultsKey.duckWhileDictating) }
    }

    /// Seconds a modifier hold must last before it counts (accidental-tap filter).
    static var holdThreshold: Double {
        get { clamp(defaults.object(forKey: DefaultsKey.holdThreshold) as? Double ?? 0.3, 0.1, 1.0) }
        set { defaults.set(newValue, forKey: DefaultsKey.holdThreshold) }
    }

    static var mouseButton: Int? {
        get {
            guard let value = defaults.object(forKey: DefaultsKey.mouseButton) as? Int, value >= 2
            else { return nil }
            return value
        }
        set {
            if let newValue {
                defaults.set(newValue, forKey: DefaultsKey.mouseButton)
            } else {
                defaults.removeObject(forKey: DefaultsKey.mouseButton)
            }
        }
    }

    /// CoreAudio UID of the preferred microphone; nil = system default.
    static var microphoneUID: String? {
        get { defaults.string(forKey: DefaultsKey.microphoneUID) }
        set {
            if let newValue {
                defaults.set(newValue, forKey: DefaultsKey.microphoneUID)
            } else {
                defaults.removeObject(forKey: DefaultsKey.microphoneUID)
            }
        }
    }

    // MARK: Polish

    static var polishEnabled: Bool {
        get { defaults.object(forKey: DefaultsKey.polishEnabled) as? Bool ?? true }
        set { defaults.set(newValue, forKey: DefaultsKey.polishEnabled) }
    }

    static var polishTimeout: Double {
        get { clamp(defaults.object(forKey: DefaultsKey.polishTimeout) as? Double ?? 1.5, 0.5, 5.0) }
        set { defaults.set(newValue, forKey: DefaultsKey.polishTimeout) }
    }

    static var polishMaxChars: Int {
        get { clamp(defaults.object(forKey: DefaultsKey.polishMaxChars) as? Int ?? 700, 100, 4000) }
        set { defaults.set(newValue, forKey: DefaultsKey.polishMaxChars) }
    }

    /// "auto" (match the target app), "casual", or "neutral".
    static var polishTone: String {
        get { defaults.string(forKey: DefaultsKey.polishTone) ?? "auto" }
        set { defaults.set(newValue, forKey: DefaultsKey.polishTone) }
    }

    static var polishToneOverride: ToneHint? {
        switch polishTone {
        case "casual": return .casual
        case "neutral": return .neutral
        default: return nil
        }
    }

    static var spokenPunctuation: Bool {
        get { defaults.object(forKey: DefaultsKey.spokenPunctuation) as? Bool ?? false }
        set { defaults.set(newValue, forKey: DefaultsKey.spokenPunctuation) }
    }

    // MARK: Insertion

    /// "auto" (AX with paste fallback), "paste", or "type".
    static var insertMethod: String {
        get { defaults.string(forKey: DefaultsKey.insertMethod) ?? "auto" }
        set { defaults.set(newValue, forKey: DefaultsKey.insertMethod) }
    }

    static var restoreDelayMs: Int {
        get { clamp(defaults.object(forKey: DefaultsKey.restoreDelayMs) as? Int ?? 300, 50, 1500) }
        set { defaults.set(newValue, forKey: DefaultsKey.restoreDelayMs) }
    }

    static var inserterConfiguration: InserterConfiguration {
        let order: [InsertionStrategy]
        switch insertMethod {
        case "paste": order = [.paste]
        case "type": order = [.type]
        default: order = [.ax, .paste]
        }
        return InserterConfiguration(
            strategyOrder: order,
            pasteboardRestoreDelay: Double(restoreDelayMs) / 1000.0
        )
    }

    // MARK: Dictionary

    static var dictionaryURL: URL {
        URL(fileURLWithPath: NSString(
            string: "~/Library/Application Support/LocalFlow/dictionary.json"
        ).expandingTildeInPath)
    }

    static func loadDictionary() -> PersonalDictionary {
        var dictionary = (try? PersonalDictionary.load(from: dictionaryURL)) ?? PersonalDictionary()
        dictionary.spokenPunctuationEnabled = spokenPunctuation
        return dictionary
    }

    static func saveDictionary(_ dictionary: PersonalDictionary) {
        let dir = dictionaryURL.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? dictionary.save(to: dictionaryURL)
    }

    private static func clamp<T: Comparable>(_ value: T, _ low: T, _ high: T) -> T {
        min(max(value, low), high)
    }
}
