import SwiftUI
import AppKit
import ServiceManagement
import LFPolish

/// Settings window in the native System-Settings idiom: an
/// NSTabViewController with toolbar-style tabs. (SwiftUI's TabView in a
/// plain NSWindow renders as a collapsed "navigation tab bar" overflow menu
/// on macOS 26 — unusable for settings.)
@MainActor
final class SettingsWindowController {
    static let shared = SettingsWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let tabs = NSTabViewController()
            tabs.tabStyle = .toolbar

            func add(_ title: String, _ symbol: String, _ view: some View) {
                let host = NSHostingController(
                    rootView: view
                        .frame(width: 560)
                        .fixedSize(horizontal: false, vertical: true)
                )
                host.title = title
                host.sizingOptions = .preferredContentSize
                let item = NSTabViewItem(viewController: host)
                item.label = title
                item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
                tabs.addTabViewItem(item)
            }

            add("General", "gearshape", GeneralSettingsTab())
            add("Dictation", "mic", DictationSettingsTab())
            add("Polish", "wand.and.stars", PolishSettingsTab())
            add("Dictionary", "character.book.closed", DictionarySettingsTab())
            add("Insertion", "text.cursor", InsertionSettingsTab())
            add("About", "info.circle", AboutTab())

            let w = NSWindow(contentViewController: tabs)
            w.styleMask = [.titled, .closable]
            w.title = "LocalFlow Settings"
            w.toolbarStyle = .preference
            w.isReleasedWhenClosed = false
            w.center()
            window = w
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }
}

/// Local mirror of LFCapture's hotkey choice, persisted as {"key": "fn"}
/// JSON under DefaultsKey.hotkeyConfig; bridged in DictationCoordinator.
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

/// Push a settings change into the running pipeline.
@MainActor
private func apply(restartCapture: Bool = false) {
    DictationCoordinator.shared.applySettings(restartCapture: restartCapture)
}

// MARK: - General

private struct GeneralSettingsTab: View {
    @State private var launchAtLogin = SMAppService.mainApp.status == .enabled
    @State private var loginItemError: String?
    @State private var hudEnabled = AppSettings.hudEnabled
    @State private var historyLimit = AppSettings.historyLimit

    var body: some View {
        Form {
            Section {
                Toggle("Launch at login", isOn: $launchAtLogin)
                    .onChange(of: launchAtLogin) { _, enabled in updateLoginItem(enabled) }
                if let loginItemError {
                    Text(loginItemError).font(.caption).foregroundStyle(.red)
                }
                Toggle("Show recording indicator (HUD)", isOn: $hudEnabled)
                    .onChange(of: hudEnabled) { _, value in
                        AppSettings.hudEnabled = value
                    }
                Text("The floating lozenge at the bottom of the screen while you dictate.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("History") {
                Stepper("Keep last \(historyLimit) transcripts", value: $historyLimit, in: 0...50)
                    .onChange(of: historyLimit) { _, value in
                        AppSettings.historyLimit = value
                        apply()
                    }
                Text("Shown in the menu bar History submenu; click an entry to copy it.")
                    .font(.caption).foregroundStyle(.secondary)
                Button("Clear History Now") {
                    DictationCoordinator.shared.clearHistory()
                }
            }
        }
        .formStyle(.grouped)
    }

    private func updateLoginItem(_ enabled: Bool) {
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

// MARK: - Dictation

private struct DictationSettingsTab: View {
    @State private var hotkey: HotkeyChoice = .load()
    @State private var mouseButton: Int? = AppSettings.mouseButton
    @State private var isDetectingMouseButton = false
    @State private var mouseMonitors: [Any] = []
    @State private var holdThreshold = AppSettings.holdThreshold
    @State private var keepMicWarm = AppSettings.keepMicWarm

    var body: some View {
        Form {
            Section("Push to Talk") {
                Picker("Hold-to-talk key:", selection: $hotkey) {
                    ForEach(HotkeyChoice.allCases) { choice in
                        Text(choice.label).tag(choice)
                    }
                }
                .onChange(of: hotkey) { _, newValue in
                    newValue.save()
                    apply(restartCapture: true)
                }

                LabeledContent("Mouse button:") {
                    HStack {
                        Text(isDetectingMouseButton
                             ? "Press a mouse button…"
                             : mouseButton.map { "Button \($0 + 1)" } ?? "Off")
                            .foregroundStyle(isDetectingMouseButton ? .orange : .primary)
                        if isDetectingMouseButton {
                            Button("Cancel") { stopDetectingMouseButton() }
                        } else {
                            Button(mouseButton == nil ? "Detect…" : "Change…") {
                                startDetectingMouseButton()
                            }
                            if mouseButton != nil {
                                Button("Remove") { setMouseButton(nil) }
                            }
                        }
                    }
                }
                Text("A middle or side mouse button works as a second push-to-talk trigger.")
                    .font(.caption).foregroundStyle(.secondary)

                LabeledContent("Ignore taps shorter than:") {
                    HStack {
                        Slider(value: $holdThreshold, in: 0.1...1.0, step: 0.05)
                            .frame(width: 180)
                        Text(String(format: "%.2fs", holdThreshold))
                            .monospacedDigit()
                            .frame(width: 48, alignment: .trailing)
                    }
                }
                .onChange(of: holdThreshold) { _, value in
                    AppSettings.holdThreshold = value
                    apply(restartCapture: true)
                }
                Text("Accidental-tap filter: holds shorter than this are cancelled.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("Microphone") {
                Toggle("Keep microphone warm between dictations", isOn: $keepMicWarm)
                    .onChange(of: keepMicWarm) { _, value in
                        AppSettings.keepMicWarm = value
                        apply(restartCapture: true)
                    }
                Text("""
                    Faster start with Bluetooth mics (AirPods), and the first \
                    syllable is never clipped — but macOS shows the orange \
                    microphone indicator the whole time the app is listening.
                    """)
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    private func startDetectingMouseButton() {
        guard !isDetectingMouseButton else { return }
        isDetectingMouseButton = true

        let handle: (Int) -> Void = { button in
            Task { @MainActor in
                guard button >= 2 else { return }
                setMouseButton(button)
                stopDetectingMouseButton()
            }
        }
        if let local = NSEvent.addLocalMonitorForEvents(matching: .otherMouseDown, handler: { event in
            handle(event.buttonNumber)
            return event
        }) {
            mouseMonitors.append(local)
        }
        if let global = NSEvent.addGlobalMonitorForEvents(matching: .otherMouseDown, handler: { event in
            handle(event.buttonNumber)
        }) {
            mouseMonitors.append(global)
        }
    }

    private func stopDetectingMouseButton() {
        isDetectingMouseButton = false
        mouseMonitors.forEach { NSEvent.removeMonitor($0) }
        mouseMonitors.removeAll()
    }

    private func setMouseButton(_ button: Int?) {
        mouseButton = button
        AppSettings.mouseButton = button
        apply(restartCapture: true)
    }
}

// MARK: - Polish

private struct PolishSettingsTab: View {
    @State private var polishEnabled = AppSettings.polishEnabled
    @State private var timeout = AppSettings.polishTimeout
    @State private var maxChars = Double(AppSettings.polishMaxChars)
    @State private var tone = AppSettings.polishTone

    var body: some View {
        Form {
            Section {
                Toggle("Polish dictated text", isOn: $polishEnabled)
                    .onChange(of: polishEnabled) { _, value in
                        AppSettings.polishEnabled = value
                        apply()
                    }
                Text("""
                    Removes filler words (um, uh), applies mid-sentence \
                    self-corrections, and tidies phrasing using S1-mini, a \
                    small on-device model. Punctuation and casing are always \
                    restored, even with polish off. If polish is slow or \
                    misbehaves, your literal words are inserted instead — \
                    text is never lost.
                    """)
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("Style") {
                Picker("Writing style:", selection: $tone) {
                    Text("Match the app (casual in chat apps)").tag("auto")
                    Text("Always casual").tag("casual")
                    Text("Always neutral").tag("neutral")
                }
                .disabled(!polishEnabled)
                .onChange(of: tone) { _, value in
                    AppSettings.polishTone = value
                    apply()
                }
            }

            Section("Limits") {
                LabeledContent("Polish time budget:") {
                    HStack {
                        Slider(value: $timeout, in: 0.5...5.0, step: 0.25)
                            .frame(width: 180)
                        Text(String(format: "%.2fs", timeout))
                            .monospacedDigit()
                            .frame(width: 48, alignment: .trailing)
                    }
                }
                .disabled(!polishEnabled)
                .onChange(of: timeout) { _, value in
                    AppSettings.polishTimeout = value
                    apply()
                }
                Text("If the model can't finish in time, the unpolished text is inserted.")
                    .font(.caption).foregroundStyle(.secondary)

                LabeledContent("Skip polish beyond:") {
                    HStack {
                        Slider(value: $maxChars, in: 100...4000, step: 100)
                            .frame(width: 180)
                        Text("\(Int(maxChars)) chars")
                            .monospacedDigit()
                            .frame(width: 72, alignment: .trailing)
                    }
                }
                .disabled(!polishEnabled)
                .onChange(of: maxChars) { _, value in
                    AppSettings.polishMaxChars = Int(value)
                    apply()
                }
                Text("Very long dictations skip the model instantly instead of burning the budget.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: - Dictionary

private struct DictionarySettingsTab: View {
    @State private var rules: [ReplacementRule] = AppSettings.loadDictionary().rules
    @State private var spokenPunctuation = AppSettings.spokenPunctuation
    @State private var selection: Int?

    var body: some View {
        Form {
            Section {
                Text("""
                    Spoken form on the left, written form on the right — e.g. \
                    "echidna cams" → "EchidnaCams". Applied to every dictation, \
                    even with polish off.
                    """)
                    .font(.caption).foregroundStyle(.secondary)

                List(selection: $selection) {
                    ForEach(Array(rules.enumerated()), id: \.offset) { index, _ in
                        HStack {
                            TextField("spoken", text: binding(for: index).spoken)
                            Image(systemName: "arrow.right").foregroundStyle(.secondary)
                            TextField("written", text: binding(for: index).written)
                        }
                        .tag(index)
                    }
                }
                .frame(minHeight: 180)

                HStack {
                    Button {
                        rules.append(ReplacementRule(spoken: "", written: ""))
                        selection = rules.count - 1
                    } label: { Image(systemName: "plus") }
                    Button {
                        if let selection, rules.indices.contains(selection) {
                            rules.remove(at: selection)
                            save()
                        }
                        selection = nil
                    } label: { Image(systemName: "minus") }
                        .disabled(selection == nil)
                    Spacer()
                    Button("Reveal File in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([AppSettings.dictionaryURL])
                    }
                }
            }

            Section {
                Toggle("Spoken punctuation commands", isOn: $spokenPunctuation)
                    .onChange(of: spokenPunctuation) { _, value in
                        AppSettings.spokenPunctuation = value
                        apply()
                    }
                Text("Saying \"comma\", \"period\", \"new line\" inserts the punctuation itself.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onDisappear { save() }
    }

    private func binding(for index: Int) -> (spoken: Binding<String>, written: Binding<String>) {
        (
            spoken: Binding(
                get: { rules.indices.contains(index) ? rules[index].spoken : "" },
                set: { if rules.indices.contains(index) { rules[index].spoken = $0; save() } }
            ),
            written: Binding(
                get: { rules.indices.contains(index) ? rules[index].written : "" },
                set: { if rules.indices.contains(index) { rules[index].written = $0; save() } }
            )
        )
    }

    private func save() {
        var dictionary = AppSettings.loadDictionary()
        dictionary.rules = rules.filter { !$0.spoken.isEmpty && !$0.written.isEmpty }
        AppSettings.saveDictionary(dictionary)
        apply()
    }
}

// MARK: - Insertion

private struct InsertionSettingsTab: View {
    @State private var method = AppSettings.insertMethod
    @State private var restoreDelay = Double(AppSettings.restoreDelayMs)

    var body: some View {
        Form {
            Section {
                Picker("Insert text by:", selection: $method) {
                    Text("Automatic (direct insert, then paste)").tag("auto")
                    Text("Paste only").tag("paste")
                    Text("Simulated typing").tag("type")
                }
                .pickerStyle(.radioGroup)
                .onChange(of: method) { _, value in
                    AppSettings.insertMethod = value
                    apply()
                }
                Text("""
                    Automatic inserts at the caret via Accessibility and falls \
                    back to paste (your clipboard is restored). Simulated \
                    typing is slower but works in unusual apps.
                    """)
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section {
                LabeledContent("Clipboard restore delay:") {
                    HStack {
                        Slider(value: $restoreDelay, in: 50...1500, step: 50)
                            .frame(width: 180)
                        Text("\(Int(restoreDelay)) ms")
                            .monospacedDigit()
                            .frame(width: 60, alignment: .trailing)
                    }
                }
                .onChange(of: restoreDelay) { _, value in
                    AppSettings.restoreDelayMs = Int(value)
                    apply()
                }
                Text("""
                    After a paste-based insert, how long to wait before putting \
                    your previous clipboard back. Raise this if slow apps \
                    (some Electron apps) paste the wrong thing.
                    """)
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: - About

private struct AboutTab: View {
    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
    }

    var body: some View {
        Form {
            Section {
                LabeledContent("LocalFlow", value: "version \(version)")
                Text("Fully local dictation — no audio or text ever leaves this Mac.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("Models") {
                LabeledContent("Speech recognition") {
                    Text("Granite Speech 5.0 TurboCTC (IBM), MLX conversion by Kyle Howells")
                        .multilineTextAlignment(.trailing)
                }
                LabeledContent("Punctuation") {
                    Text("Punctuation/truecase formatter by Kyle Howells")
                }
                LabeledContent("Text polish") {
                    Text("S1-mini by Superwhisper")
                }
                Text("All models run on-device via MLX and are cached locally after first download.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("Acknowledgements") {
                Text("""
                    Built on Granite-MLX (Kyle Howells), MLX Swift (Apple), \
                    and S1-mini by Superwhisper (Apache 2.0). Speech models \
                    © IBM, Apache 2.0.
                    """)
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}
