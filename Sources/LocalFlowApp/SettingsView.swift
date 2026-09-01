import SwiftUI
import AppKit
import ServiceManagement

/// Plain NSWindow host for settings (AppKit lifecycle; no Settings scene).
@MainActor
final class SettingsWindowController {
    static let shared = SettingsWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 440, height: 480),
                styleMask: [.titled, .closable],
                backing: .buffered,
                defer: false
            )
            w.title = "LocalFlow Settings"
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: SettingsView())
            w.center()
            window = w
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }
}

/// Local mirror of LFCapture's hotkey choice. LFCapture's HotkeyConfig type is
/// not public yet (module is a placeholder), so we keep this enum here and
/// persist a JSON value shaped like {"key": "fn"} under DefaultsKey.hotkeyConfig
/// for LFCapture to consume. NOTE for orchestrator: align raw values with
/// LFCapture's real type at integration.
enum HotkeyChoice: String, CaseIterable, Identifiable {
    case fn
    case rightCommand
    case rightOption

    var id: String { rawValue }

    var label: String {
        switch self {
        case .fn: return "Fn (Globe)"
        case .rightCommand: return "Right Command"
        case .rightOption: return "Right Option"
        }
    }

    static func load() -> HotkeyChoice {
        guard
            let data = UserDefaults.standard.data(forKey: DefaultsKey.hotkeyConfig),
            let dict = try? JSONDecoder().decode([String: String].self, from: data),
            let raw = dict["key"],
            let choice = HotkeyChoice(rawValue: raw)
        else { return .fn }
        return choice
    }

    func save() {
        if let data = try? JSONEncoder().encode(["key": rawValue]) {
            UserDefaults.standard.set(data, forKey: DefaultsKey.hotkeyConfig)
        }
    }
}

struct SettingsView: View {
    @State private var hotkey: HotkeyChoice = .load()
    @AppStorage(DefaultsKey.polishEnabled) private var polishEnabled = true
    @State private var launchAtLogin = SMAppService.mainApp.status == .enabled
    @State private var loginItemError: String?

    private var dictionaryPath: String {
        ("~/Library/Application Support/LocalFlow/dictionary.json" as NSString)
            .expandingTildeInPath
    }

    var body: some View {
        Form {
            Section("Dictation") {
                Picker("Hold-to-talk key:", selection: $hotkey) {
                    ForEach(HotkeyChoice.allCases) { choice in
                        Text(choice.label).tag(choice)
                    }
                }
                .onChange(of: hotkey) { _, newValue in
                    newValue.save()
                    DictationCoordinator.shared.restartListeningIfNeeded()
                }
                Text("Hold the key to record; release to transcribe and insert.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("Polish text (fix punctuation and phrasing)", isOn: $polishEnabled)
            }

            Section("General") {
                Toggle("Launch at login", isOn: $launchAtLogin)
                    .onChange(of: launchAtLogin) { _, enabled in
                        updateLoginItem(enabled)
                    }
                if let loginItemError {
                    Text(loginItemError)
                        .font(.caption)
                        .foregroundStyle(.red)
                }
            }

            Section("Dictionary") {
                LabeledContent("Replacements file:") {
                    Text(dictionaryPath)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                        .lineLimit(2)
                }
                Button("Reveal in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting(
                        [URL(fileURLWithPath: dictionaryPath)]
                    )
                }
                Text("Edit this JSON file to add custom word replacements.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Microphone") {
                Text("LocalFlow records from the system default input device. Change it in System Settings → Sound → Input.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 440)
        .fixedSize(horizontal: false, vertical: true)
    }

    private func updateLoginItem(_ enabled: Bool) {
        // SMAppService only works from a real .app bundle; from `swift run`
        // (bare executable) registration fails — surface that instead of crashing.
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            loginItemError = nil
        } catch {
            loginItemError = "Couldn't update login item: \(error.localizedDescription)"
            launchAtLogin = SMAppService.mainApp.status == .enabled
        }
    }
}
