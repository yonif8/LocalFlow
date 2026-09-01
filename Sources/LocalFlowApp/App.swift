import SwiftUI
import AppKit
import Observation
import os

// AppKit lifecycle (not SwiftUI scenes): SwiftUI's MenuBarExtra failed to
// materialize a status item when this SwiftPM-built bundle ran on this OS,
// so the menu bar presence is a plain NSStatusItem — deterministic and fully
// controllable. SwiftUI is still used for all window content (Settings,
// Onboarding, HUD) via NSHostingView.
//
// NOTE: SwiftPM executable — this file must not be named main.swift for the
// @main attribute to be legal.

@main
enum LocalFlowMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        // Keep a strong reference for the app's lifetime.
        objc_setAssociatedObject(app, &delegateKey, delegate, .OBJC_ASSOCIATION_RETAIN)
        app.run()
    }
}

private nonisolated(unsafe) var delegateKey: UInt8 = 0

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem?
    /// Set only by our own Quit menu item.
    private var quitRequestedByUser = false

    /// A menu-bar agent must outlive its windows: utilities like SmartClose
    /// send a quit Apple Event when an app's last window closes. Allow quit
    /// only from our own Quit item or from the system (logout/shutdown via
    /// loginwindow); refuse it from other apps.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if quitRequestedByUser { return .terminateNow }

        let logger = Logger(subsystem: "com.localflow.app", category: "lifecycle")
        guard let event = NSAppleEventManager.shared().currentAppleEvent,
              let senderDesc = event.attributeDescriptor(forKeyword: AEKeyword(keyAddressAttr)),
              let pidDesc = senderDesc.coerce(toDescriptorType: typeKernelProcessID)
        else {
            // Not an external quit event (e.g. our own programmatic terminate).
            return .terminateNow
        }

        var pid: pid_t = 0
        withUnsafeMutableBytes(of: &pid) { _ = pidDesc.data.copyBytes(to: $0) }
        if pid == ProcessInfo.processInfo.processIdentifier { return .terminateNow }

        let requester = NSRunningApplication(processIdentifier: pid)
        let bundleID = requester?.bundleIdentifier ?? "pid \(pid)"
        if bundleID == "com.apple.loginwindow" { return .terminateNow }

        // Utilities like SmartClose send quit INSTEAD of closing the window
        // the user X-ed, so honor the intent: close regular windows (not the
        // HUD panel), keep the process alive in the menu bar.
        logger.info("refused quit request from \(bundleID, privacy: .public); closing windows instead")
        for window in NSApp.windows where window.isVisible && !(window is NSPanel) {
            window.close()
        }
        return .terminateCancel
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Agent app: no Dock icon. LSUIElement covers the bundled case; this
        // covers `swift run` during development.
        NSApp.setActivationPolicy(.accessory)

        setUpStatusItem()
        DictationCoordinator.shared.startListening()
        observeCoordinatorState()

        // Debug hook so the mock pipeline can be driven headlessly:
        //   swift -e 'import Foundation
        //     DistributedNotificationCenter.default().postNotificationName(
        //       Notification.Name("com.localflow.app.simulate"), object: nil,
        //       userInfo: nil, deliverImmediately: true)
        //     RunLoop.main.run(until: Date().addingTimeInterval(0.5))'
        // Selector-based + .deliverImmediately: an LSUIElement app is never
        // "active", so default suspension behavior would hold notes forever.
        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(handleSimulateNotification),
            name: Notification.Name("com.localflow.app.simulate"),
            object: nil,
            suspensionBehavior: .deliverImmediately
        )
        // Companion debug hook: open the Settings window headlessly.
        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(handleShowSettingsNotification),
            name: Notification.Name("com.localflow.app.showSettings"),
            object: nil,
            suspensionBehavior: .deliverImmediately
        )

        // First launch (or missing permissions): show onboarding.
        let defaults = UserDefaults.standard
        let hasLaunched = defaults.bool(forKey: DefaultsKey.hasLaunchedBefore)
        if !hasLaunched || !PermissionChecker.allGranted() {
            defaults.set(true, forKey: DefaultsKey.hasLaunchedBefore)
            OnboardingWindowController.shared.show()
        }
    }

    // MARK: - Status item

    private func setUpStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.behavior = []
        if let button = item.button {
            button.image = Self.symbolImage(named: DictationCoordinator.shared.menuBarSymbolName)
            button.toolTip = "LocalFlow"
        }
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        statusItem = item

        // Diagnostics: confirm the item actually landed in the menu bar.
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(1))
            let logger = os.Logger(subsystem: "com.localflow.app", category: "statusitem")
            if let button = item.button, let window = button.window {
                logger.info("status item window=\(window.windowNumber, privacy: .public) frame=\(String(describing: window.frame), privacy: .public) visible=\(item.isVisible, privacy: .public) image=\(button.image != nil, privacy: .public)")
            } else {
                logger.error("status item has no button/window; isVisible=\(item.isVisible, privacy: .public)")
            }
        }
    }

    private static func symbolImage(named name: String) -> NSImage? {
        let image = NSImage(systemSymbolName: name, accessibilityDescription: "LocalFlow")
        image?.isTemplate = true
        return image
    }

    /// Re-render the status icon whenever the coordinator's state changes.
    /// (The onChange closure is @Sendable, so it reaches the delegate through
    /// NSApp rather than capturing non-Sendable self.)
    private func observeCoordinatorState() {
        withObservationTracking {
            _ = DictationCoordinator.shared.menuBarSymbolName
        } onChange: {
            Task { @MainActor in
                guard let delegate = NSApp.delegate as? AppDelegate else { return }
                delegate.statusItem?.button?.image =
                    Self.symbolImage(named: DictationCoordinator.shared.menuBarSymbolName)
                delegate.observeCoordinatorState()
            }
        }
    }

    // MARK: - Menu (rebuilt each time it opens)

    func menuNeedsUpdate(_ menu: NSMenu) {
        let coordinator = DictationCoordinator.shared
        menu.removeAllItems()

        let listenTitle = coordinator.isListening ? "Stop Listening" : "Start Listening"
        menu.addItem(makeItem(listenTitle, action: #selector(toggleListening), key: ""))

        let simulate = makeItem("Simulate Dictation", action: #selector(simulateDictation), key: "")
        simulate.isEnabled = coordinator.isListening
        menu.addItem(simulate)

        if let limitation = coordinator.captureLimitation {
            let info = NSMenuItem(title: limitation, action: nil, keyEquivalent: "")
            info.isEnabled = false
            menu.addItem(info)
        }

        menu.addItem(.separator())

        let historyItem = NSMenuItem(title: "History", action: nil, keyEquivalent: "")
        let historyMenu = NSMenu(title: "History")
        if coordinator.history.isEmpty {
            let empty = NSMenuItem(title: "No transcripts yet", action: nil, keyEquivalent: "")
            empty.isEnabled = false
            historyMenu.addItem(empty)
        } else {
            for transcript in coordinator.history {
                let entry = makeItem(transcript.menuTitle, action: #selector(copyTranscript(_:)), key: "")
                entry.representedObject = transcript.text
                entry.toolTip = "Click to copy"
                historyMenu.addItem(entry)
            }
        }
        historyItem.submenu = historyMenu
        menu.addItem(historyItem)

        menu.addItem(.separator())
        menu.addItem(makeItem("Settings…", action: #selector(openSettingsWindow), key: ","))
        menu.addItem(makeItem("Permissions…", action: #selector(openPermissions), key: ""))
        menu.addItem(.separator())
        menu.addItem(makeItem("Quit LocalFlow", action: #selector(quit), key: "q"))
    }

    private func makeItem(_ title: String, action: Selector, key: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
        item.target = self
        return item
    }

    // MARK: - Actions

    @objc private func toggleListening() {
        let coordinator = DictationCoordinator.shared
        if coordinator.isListening {
            coordinator.stopListening()
        } else {
            coordinator.startListening()
        }
    }

    @objc private func simulateDictation() {
        DictationCoordinator.shared.simulateDictation()
    }

    @objc private func copyTranscript(_ sender: NSMenuItem) {
        guard let text = sender.representedObject as? String else { return }
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }

    @objc private func openSettingsWindow() {
        SettingsWindowController.shared.show()
    }

    @objc private func openPermissions() {
        OnboardingWindowController.shared.show()
    }

    @objc private func quit() {
        quitRequestedByUser = true
        NSApp.terminate(nil)
    }

    @objc private func handleSimulateNotification(_ note: Notification) {
        Task { @MainActor in
            DictationCoordinator.shared.simulateDictation()
        }
    }

    @objc private func handleShowSettingsNotification(_ note: Notification) {
        Task { @MainActor in
            SettingsWindowController.shared.show()
        }
    }
}

enum DefaultsKey {
    static let hasLaunchedBefore = "LFHasLaunchedBefore"
    static let polishEnabled = "LFPolishEnabled"
    /// JSON-encoded `HotkeyConfig`-shaped value ({"key": "fn" | "rightCommand" | "rightOption"}).
    /// LFCapture can read this at startup; the local mirror enum lives in
    /// SettingsView.swift and is bridged to LFCapture.HotkeyKey in
    /// DictationCoordinator.swift.
    static let hotkeyConfig = "LFHotkeyConfig"
}
