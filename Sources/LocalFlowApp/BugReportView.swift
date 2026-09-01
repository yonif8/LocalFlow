import SwiftUI
import AppKit

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
                Text("Opens a prefilled GitHub issue — nothing is sent until you submit it there.")
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

        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev"
        let os = ProcessInfo.processInfo.operatingSystemVersionString
        let title = report.count > 60 ? String(report.prefix(57)) + "…" : report
        let body = """
        \(report)

        ---
        LocalFlow \(version) · \(os) · engine: \(AppSettings.speechEngine)
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
