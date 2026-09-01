import SwiftUI
import AppKit

/// Wispr-style floating lozenge at the bottom-center of the screen, shown only
/// while recording/processing. Non-activating, ignores mouse, joins all Spaces.
@MainActor
final class HUDController {
    static let shared = HUDController()

    private var panel: HUDPanel?

    func show() {
        if panel == nil {
            panel = HUDPanel()
        }
        guard let panel else { return }
        position(panel)
        panel.alphaValue = 0
        panel.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.15
            panel.animator().alphaValue = 1
        }
    }

    func hide() {
        guard let panel, panel.isVisible else { return }
        NSAnimationContext.runAnimationGroup({ context in
            context.duration = 0.2
            panel.animator().alphaValue = 0
        }, completionHandler: {
            Task { @MainActor in
                // Only actually hide if a show() didn't race the fade-out.
                if panel.alphaValue == 0 {
                    panel.orderOut(nil)
                }
            }
        })
    }

    private func position(_ panel: NSPanel) {
        guard let screen = NSScreen.main else { return }
        let size = panel.frame.size
        let x = screen.frame.midX - size.width / 2
        let y = screen.frame.minY + 64
        panel.setFrameOrigin(NSPoint(x: x, y: y))
    }
}

private final class HUDPanel: NSPanel {
    init() {
        super.init(
            contentRect: NSRect(x: 0, y: 0, width: 280, height: 56),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        isFloatingPanel = true
        level = .statusBar
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        ignoresMouseEvents = true
        isOpaque = false
        backgroundColor = .clear
        hasShadow = true
        hidesOnDeactivate = false
        isReleasedWhenClosed = false
        contentView = NSHostingView(rootView: HUDView())
    }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

private struct HUDView: View {
    @State private var coordinator = DictationCoordinator.shared

    var body: some View {
        HStack(spacing: 10) {
            switch coordinator.state {
            case .recording:
                Image(systemName: "mic.fill")
                    .foregroundStyle(.red)
                LevelMeter(level: coordinator.level)
            case .processing:
                ProgressView()
                    .controlSize(.small)
                Text("processing…")
                    .foregroundStyle(.secondary)
            case .error(let message):
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.yellow)
                Text(message)
                    .lineLimit(2)
                    .font(.caption)
            case .idle:
                EmptyView()
            }
        }
        .font(.system(size: 13, weight: .medium))
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .frame(width: 280, height: 56)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay(Capsule().strokeBorder(.white.opacity(0.15), lineWidth: 1))
    }
}

/// Bar-style live level meter driven by CaptureEvent.level (0...1).
private struct LevelMeter: View {
    let level: Float
    private let barCount = 18

    var body: some View {
        HStack(spacing: 3) {
            ForEach(0..<barCount, id: \.self) { index in
                Capsule()
                    .fill(.white.opacity(0.9))
                    .frame(width: 3, height: barHeight(index))
            }
        }
        .frame(height: 28)
        .animation(.linear(duration: 0.05), value: level)
    }

    private func barHeight(_ index: Int) -> CGFloat {
        // Center-weighted bars with per-bar variation so motion reads as speech.
        let center = Double(barCount - 1) / 2
        let distance = abs(Double(index) - center) / center
        let weight = 1.0 - 0.65 * distance * distance
        let jitter = 0.75 + 0.25 * sin(Double(index) * 2.4 + Double(level) * 21)
        let height = 4 + CGFloat(Double(level) * 24 * weight * jitter)
        return max(4, min(28, height))
    }
}
