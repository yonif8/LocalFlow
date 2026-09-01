import SwiftUI
import AppKit
import ServiceManagement
import LFEngine
import LFPolish

/// Everything about this install that helps debugging, and nothing private:
/// states, settings, and counts — never dictation history or dictionary
/// contents.
enum BugDiagnostics {
    static func summary() -> String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "dev"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        var model = ""
        var size = size_t()
        sysctlbyname("hw.model", nil, &size, nil, 0)
        var buffer = [CChar](repeating: 0, count: size)
        sysctlbyname("hw.model", &buffer, &size, nil, 0)
        model = String(cString: buffer)

        let dictionary = AppSettings.loadDictionary()
        let mic = AppSettings.microphoneUID.map { uid in
            MicDevice.available().first(where: { $0.uid == uid })?.name ?? "disconnected (\(uid))"
        } ?? "system default"

        return """
        LocalFlow \(version) (build \(build)) · \(ProcessInfo.processInfo.operatingSystemVersionString) · \(model)

        Permissions: mic=\(PermissionChecker.microphone().label), \
        input-monitoring=\(PermissionChecker.inputMonitoring().label), \
        accessibility=\(PermissionChecker.accessibility().label)
        Models: granite-speech=\(EngineModelLocations.isSpeechModelDownloaded() ? "downloaded" : "missing"), \
        punctuation=\(EngineModelLocations.isPunctuationModelDownloaded() ? "downloaded" : "missing"), \
        s1-mini=\(PolishModelStore.isModelDownloaded ? "downloaded" : "missing")
        Engine: \(AppSettings.speechEngine)
        Hotkey: \(HotkeyChoice.load().rawValue), mouse-button=\(AppSettings.mouseButton.map { "\($0 + 1)" } ?? "off"), \
        hold-threshold=\(String(format: "%.2fs", AppSettings.holdThreshold)), mic-warm=\(AppSettings.keepMicWarm)
        Microphone: \(mic)
        Polish: enabled=\(AppSettings.polishEnabled), tone=\(AppSettings.polishTone), \
        timeout=\(String(format: "%.2fs", AppSettings.polishTimeout)), max-chars=\(AppSettings.polishMaxChars), \
        spoken-punctuation=\(AppSettings.spokenPunctuation)
        Insertion: method=\(AppSettings.insertMethod), restore-delay=\(AppSettings.restoreDelayMs)ms
        General: hud=\(AppSettings.hudEnabled), history-limit=\(AppSettings.historyLimit), \
        launch-at-login=\(SMAppService.mainApp.status == .enabled), \
        dictionary-rules=\(dictionary.rules.count)
        """
    }
}

/// Minimal bug reporter: one text box, Enter submits. Composes a prefilled
/// GitHub new-issue page in the browser — nothing is sent silently, and no
/// credentials live in the app.
@MainActor
final class BugReportWindowController {
    static let shared = BugReportWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 460, height: 150),
                styleMask: [.titled, .closable],
                backing: .buffered,
                defer: false
            )
            w.title = "Report a Bug"
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: BugReportView(onDone: { [weak self] in
                self?.window?.close()
            }))
            w.center()
            window = w
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }
}

private struct BugReportView: View {
    let onDone: () -> Void
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("What went wrong? Dictate or type, then press Return.")
                .font(.callout)
            TextField("e.g. Dictation into Mail inserted nothing…", text: $text, axis: .vertical)
                .textFieldStyle(.roundedBorder)
                .lineLimit(3...6)
                .onSubmit(submit)
            HStack {
                Text("Opens a prefilled GitHub issue with your app settings attached — nothing is sent until you submit it there.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Report", action: submit)
                    .keyboardShortcut(.defaultAction)
                    .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(16)
        .frame(width: 460)
    }

    private func submit() {
        let report = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !report.isEmpty else { return }

        let title = report.count > 60 ? String(report.prefix(57)) + "…" : report
        let body = """
        \(report)

        ---
        <details><summary>Diagnostics</summary>

        ```
        \(BugDiagnostics.summary())
        ```
        </details>
        """

        var components = URLComponents(string: "https://github.com/yonif8/LocalFlow/issues/new")!
        components.queryItems = [
            URLQueryItem(name: "title", value: title),
            URLQueryItem(name: "body", value: body),
        ]
        if let url = components.url {
            NSWorkspace.shared.open(url)
        }
        text = ""
        onDone()
    }
}
