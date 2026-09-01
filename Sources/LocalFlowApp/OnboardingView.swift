import SwiftUI
import AppKit
import AVFoundation
import ApplicationServices
import IOKit.hid

/// Non-prompting permission checks (safe to poll on a timer).
enum PermissionChecker {
    enum Status {
        case granted
        case denied
        case notDetermined

        var label: String {
            switch self {
            case .granted: return "Granted"
            case .denied: return "Denied"
            case .notDetermined: return "Not requested"
            }
        }

        var symbol: String {
            switch self {
            case .granted: return "checkmark.circle.fill"
            case .denied: return "xmark.circle.fill"
            case .notDetermined: return "questionmark.circle.fill"
            }
        }

        var color: Color {
            switch self {
            case .granted: return .green
            case .denied: return .red
            case .notDetermined: return .orange
            }
        }
    }

    static func microphone() -> Status {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return .granted
        case .denied, .restricted: return .denied
        case .notDetermined: return .notDetermined
        @unknown default: return .notDetermined
        }
    }

    static func inputMonitoring() -> Status {
        switch IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) {
        case kIOHIDAccessTypeGranted: return .granted
        case kIOHIDAccessTypeDenied: return .denied
        default: return .notDetermined
        }
    }

    static func accessibility() -> Status {
        // AXIsProcessTrusted does not prompt; it can't distinguish
        // denied from not-yet-requested.
        AXIsProcessTrusted() ? .granted : .notDetermined
    }

    static func allGranted() -> Bool {
        microphone() == .granted
            && inputMonitoring() == .granted
            && accessibility() == .granted
    }
}

/// Plain NSWindow host (agent app; avoids Window-scene activation quirks).
@MainActor
final class OnboardingWindowController {
    static let shared = OnboardingWindowController()

    private var window: NSWindow?

    func show() {
        if window == nil {
            let w = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 460, height: 380),
                styleMask: [.titled, .closable],
                backing: .buffered,
                defer: false
            )
            w.title = "Welcome to LocalFlow"
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: OnboardingView())
            w.center()
            window = w
            // Tear the view down on close: its 1.5s permission poll fires
            // TCC IPC forever if the hosting view outlives the window.
            NotificationCenter.default.addObserver(
                forName: NSWindow.willCloseNotification, object: w, queue: .main
            ) { [weak self] _ in
                Task { @MainActor in
                    self?.window?.contentView = nil
                    self?.window = nil
                }
            }
        }
        NSApp.activate(ignoringOtherApps: true)
        window?.makeKeyAndOrderFront(nil)
    }
}

struct OnboardingView: View {
    @State private var micStatus = PermissionChecker.microphone()
    @State private var inputStatus = PermissionChecker.inputMonitoring()
    @State private var axStatus = PermissionChecker.accessibility()

    private let timer = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 12) {
                Image(systemName: "waveform.circle.fill")
                    .font(.system(size: 40))
                    .foregroundStyle(.tint)
                VStack(alignment: .leading) {
                    Text("LocalFlow").font(.title2.bold())
                    Text("Fully local dictation. Hold a key, speak, release — text appears where your cursor is.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            Text("LocalFlow needs three permissions to work:")
                .font(.headline)

            PermissionRow(
                title: "Microphone",
                detail: "Records your voice while the hotkey is held.",
                status: micStatus,
                buttonTitle: micStatus == .notDetermined ? "Request Access" : "Open System Settings"
            ) {
                if micStatus == .notDetermined {
                    AVCaptureDevice.requestAccess(for: .audio) { _ in }
                } else {
                    openPane("Privacy_Microphone")
                }
            }

            PermissionRow(
                title: "Input Monitoring",
                detail: "Detects the hold-to-talk hotkey in any app.",
                status: inputStatus,
                buttonTitle: "Open System Settings"
            ) {
                openPane("Privacy_ListenEvent")
            }

            PermissionRow(
                title: "Accessibility",
                detail: "Types the transcribed text into the frontmost app.",
                status: axStatus,
                buttonTitle: "Open System Settings"
            ) {
                openPane("Privacy_Accessibility")
            }

            Divider()

            Text("Tip: after granting a permission, macOS may require relaunching LocalFlow for it to take effect.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(24)
        .frame(width: 460)
        .onReceive(timer) { _ in
            micStatus = PermissionChecker.microphone()
            inputStatus = PermissionChecker.inputMonitoring()
            axStatus = PermissionChecker.accessibility()
        }
    }

    private func openPane(_ anchor: String) {
        let url = "x-apple.systempreferences:com.apple.preference.security?\(anchor)"
        if let u = URL(string: url) {
            NSWorkspace.shared.open(u)
        }
    }
}

private struct PermissionRow: View {
    let title: String
    let detail: String
    let status: PermissionChecker.Status
    let buttonTitle: String
    let action: () -> Void

    var body: some View {
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: status.symbol)
                .foregroundStyle(status.color)
                .font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(title).font(.body.weight(.semibold))
                    Text(status.label)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if status != .granted {
                Button(buttonTitle, action: action)
                    .controlSize(.small)
            }
        }
    }
}
