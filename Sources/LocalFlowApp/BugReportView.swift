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
        accessibility=\(PermissionChecker.accessibility().label), \
        screen-recording=\(PermissionChecker.screenRecording().label)
        Models: parakeet=\(ParakeetTranscriber.isModelDownloaded ? "downloaded" : "missing"), \
        s1-mini=\(PolishModelStore.isModelDownloaded ? "downloaded" : "missing")
        Hotkey: \(HotkeyChoice.load().rawValue), mouse-button=\(AppSettings.mouseButton.map { "\($0 + 1)" } ?? "off"), \
        hold-threshold=\(String(format: "%.2fs", AppSettings.holdThreshold)), mic-warm=\(AppSettings.keepMicWarm)
        Microphone: \(mic)
        Polish: enabled=\(AppSettings.polishEnabled), tone=\(AppSettings.polishTone), \
        timeout=\(String(format: "%.2fs", AppSettings.polishTimeout)), max-chars=\(AppSettings.polishMaxChars), \
        spoken-punctuation=\(AppSettings.spokenPunctuation), \
        screen-terminology=\(AppSettings.screenTerminologyEnabled), \
        learned-terms=\(LearnedTerminologyStore.load().count)
        Insertion: method=\(AppSettings.insertMethod), restore-delay=\(AppSettings.restoreDelayMs)ms
        General: hud=\(AppSettings.hudEnabled), history-limit=\(AppSettings.historyLimit), \
        launch-at-login=\(SMAppService.mainApp.status == .enabled), \
        dictionary-rules=\(dictionary.rules.count)
        """
    }
}

/// Bug reporter as a modern modal overlay: the whole screen dims, a large
/// HUD-material card floats centered. One text box, Return submits (opens a
/// prefilled GitHub issue — nothing sent silently), Esc or clicking the
/// dimmed area dismisses.
@MainActor
final class BugReportWindowController {
    static let shared = BugReportWindowController()

    private var window: NSWindow?

    func show() {
        guard let screen = NSScreen.main else { return }
        if window == nil {
            let w = KeyableOverlayWindow(
                contentRect: screen.frame,
                styleMask: [.borderless],
                backing: .buffered,
                defer: false
            )
            w.isOpaque = false
            w.backgroundColor = .clear
            w.level = .modalPanel
            w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            w.isReleasedWhenClosed = false
            w.hasShadow = false
            w.contentView = NSHostingView(rootView: BugReportView(onDone: { [weak self] in
                self?.dismiss()
            }))
            window = w
        }
        window?.setFrame(screen.frame, display: true)
        window?.alphaValue = 0
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.18
            window?.animator().alphaValue = 1
        }
    }

    func dismiss() {
        guard let window else { return }
        NSAnimationContext.runAnimationGroup({ context in
            context.duration = 0.15
            window.animator().alphaValue = 0
        }, completionHandler: {
            Task { @MainActor in
                window.orderOut(nil)
            }
        })
    }
}

/// Borderless windows refuse key status by default; typing needs it.
private final class KeyableOverlayWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override func cancelOperation(_ sender: Any?) {
        BugReportWindowController.shared.dismiss()
    }
}

private struct BugReportView: View {
    let onDone: () -> Void
    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        ZStack {
            // Dimmed backdrop — click anywhere outside the card to dismiss.
            Color.black.opacity(0.45)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { onDone() }

            VStack(spacing: 0) {
                HStack(spacing: 14) {
                    Image(systemName: "ladybug.fill")
                        .font(.system(size: 24))
                        .foregroundStyle(.tint)
                    TextField("Report a bug — what went wrong?", text: $text, axis: .vertical)
                        .textFieldStyle(.plain)
                        .font(.title2)
                        .lineLimit(1...8)
                        .focused($focused)
                        .onSubmit(submit)
                    Button(action: submit) {
                        Image(systemName: "arrow.up")
                            .font(.system(size: 16, weight: .bold))
                            .frame(width: 36, height: 36)
                    }
                    .buttonStyle(.borderedProminent)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                    .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 18)

                Divider()

                HStack {
                    Text("Dictate or type, then press Return — opens a prefilled GitHub issue with your app settings attached. Nothing is sent until you submit it there.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 20)
                    Text("esc to dismiss")
                        .font(.callout)
                        .foregroundStyle(.tertiary)
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
                .background(Color(nsColor: .underPageBackgroundColor))
            }
            .frame(width: 760)
            .background(Color(nsColor: .windowBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 18))
            .overlay(
                RoundedRectangle(cornerRadius: 18)
                    .strokeBorder(Color.primary.opacity(0.12), lineWidth: 1)
            )
            .shadow(color: .black.opacity(0.35), radius: 40, y: 12)
            .onAppear {
                text = ""
                focused = true
            }
            // Esc needs a target when the window's cancelOperation is bypassed
            // by the focused text field.
            .background(
                Button("") { onDone() }
                    .keyboardShortcut(.cancelAction)
                    .opacity(0)
            )
        }
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
