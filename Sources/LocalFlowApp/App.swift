import SwiftUI
import AppKit
import Observation
import os
import Sparkle

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

    /// Sparkle auto-updates. Retained for the app's lifetime; startingUpdater
    /// is only true when the bundle carries a feed URL + EdDSA public key
    /// (i.e. a make-app.sh --version build). Dev builds (`swift run`, plain
    /// dist builds without keys) skip the updater entirely so Sparkle never
    /// complains about a missing/invalid configuration at launch.
    private let updaterController: SPUStandardUpdaterController? = {
        let info = Bundle.main.infoDictionary
        guard let feed = info?["SUFeedURL"] as? String, !feed.isEmpty,
              let key = info?["SUPublicEDKey"] as? String, !key.isEmpty else {
            os.Logger(subsystem: "com.localflow.app", category: "updater")
                .info("Sparkle disabled: no SUFeedURL/SUPublicEDKey in Info.plist (dev build)")
            return nil
        }
        return SPUStandardUpdaterController(
            startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
    }()

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
        // Companion debug hook: trigger a Sparkle update check headlessly
        // (same UI path as the "Check for Updates…" menu item).
        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(handleCheckForUpdatesNotification),
            name: Notification.Name("com.localflow.app.checkForUpdates"),
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
            // Keep it short: the widest item sets the whole menu's width.
            let info = makeItem("⚠︎ Hotkey off — open Permissions…", action: #selector(openPermissions), key: "")
            info.toolTip = limitation
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
        if let updaterController {
            let update = NSMenuItem(
                title: "Check for Updates…",
                action: #selector(SPUStandardUpdaterController.checkForUpdates(_:)),
                keyEquivalent: "")
            update.target = updaterController
            menu.addItem(update)
        }
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
        NSApp.terminate(nil)
    }

    @objc private func handleSimulateNotification(_ note: Notification) {
        Task { @MainActor in
            DictationCoordinator.shared.simulateDictation()
        }
    }

    @objc private func handleCheckForUpdatesNotification(_ note: Notification) {
        Task { @MainActor in
            guard let delegate = NSApp.delegate as? AppDelegate else { return }
            delegate.updaterController?.checkForUpdates(nil)
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
    /// CGEvent button number of the secondary push-to-talk mouse button;
    /// absent/negative = off. (2 = middle, 3+ = side buttons.)
    static let mouseButton = "LFMouseButton"
    static let hudEnabled = "LFHUDEnabled"                     // default true
    static let keepMicWarm = "LFKeepMicWarm"                   // default false
    static let holdThreshold = "LFHoldThreshold"               // seconds, default 0.3
    static let polishTimeout = "LFPolishTimeout"               // seconds, default 1.5
    static let polishMaxChars = "LFPolishMaxChars"             // default 700
    static let polishTone = "LFPolishTone"                     // "auto" | "casual" | "neutral"
    static let insertMethod = "LFInsertMethod"                 // "auto" | "paste" | "type"
    static let restoreDelayMs = "LFRestoreDelayMs"             // default 300
    static let historyLimit = "LFHistoryLimit"                 // default 10
    static let spokenPunctuation = "LFSpokenPunctuation"       // default false
    static let microphoneUID = "LFMicrophoneUID"               // absent = system default
    static let speechEngine = "LFSpeechEngine"                 // "granite" | "parakeet"
    /// JSON-encoded `HotkeyConfig`-shaped value ({"key": "fn" | "rightCommand" | "rightOption"}).
    /// LFCapture can read this at startup; the local mirror enum lives in
    /// SettingsView.swift and is bridged to LFCapture.HotkeyKey in
    /// DictationCoordinator.swift.
    static let hotkeyConfig = "LFHotkeyConfig"
}
