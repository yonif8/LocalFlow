import CoreGraphics
import Foundation

/// CGEventTap-based hotkey monitor.
///
/// Uses a LISTEN-ONLY tap (`.listenOnly`) so only Input Monitoring permission
/// is required (not Accessibility) and the Fn event is never consumed —
/// consuming it would break the system's double-Fn dictation shortcut.
///
/// The tap runs on a dedicated thread with its own CFRunLoop, and re-enables
/// itself when macOS disables it (`tapDisabledByTimeout` / `tapDisabledByUserInput`).
final class ErrorBox: @unchecked Sendable {
    var error: CaptureError?
}

final class EventTapHotkeyMonitor: @unchecked Sendable {
    private let keys: [HotkeyKey]

    /// Callbacks fire on the tap thread.
    var onKeyDown: (() -> Void)?
    var onKeyUp: (() -> Void)?
    var onEscape: (() -> Void)?

    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var tapRunLoop: CFRunLoop?
    private var thread: Thread?
    /// The trigger currently held, if any. Only its release ends the hold —
    /// a second trigger pressed mid-hold is ignored.
    private var activeKey: HotkeyKey?

    init(keys: [HotkeyKey]) {
        self.keys = keys
    }

    convenience init(key: HotkeyKey) {
        self.init(keys: [key])
    }

    func start() throws {
        guard thread == nil else { return }

        let ready = DispatchSemaphore(value: 0)
        // Written on the tap thread before `ready.signal()`, read after `ready.wait()`.
        let startErrorBox = ErrorBox()

        let t = Thread { [weak self] in
            guard let self else { ready.signal(); return }

            var mask: CGEventMask =
                (CGEventMask(1) << CGEventType.flagsChanged.rawValue)
                | (CGEventMask(1) << CGEventType.keyDown.rawValue)
                | (CGEventMask(1) << CGEventType.keyUp.rawValue)
            // Mouse-button triggers (middle/side buttons) arrive as
            // otherMouseDown/otherMouseUp; only subscribe when configured.
            if self.keys.contains(where: {
                if case .mouseButton = $0 { return true } else { return false }
            }) {
                mask |= (CGEventMask(1) << CGEventType.otherMouseDown.rawValue)
                    | (CGEventMask(1) << CGEventType.otherMouseUp.rawValue)
            }

            let callback: CGEventTapCallBack = { _, type, event, userInfo in
                guard let userInfo else { return Unmanaged.passUnretained(event) }
                let monitor = Unmanaged<EventTapHotkeyMonitor>.fromOpaque(userInfo).takeUnretainedValue()
                monitor.handle(type: type, event: event)
                // Listen-only tap: the return value is ignored, but pass the event through.
                return Unmanaged.passUnretained(event)
            }

            guard let tap = CGEvent.tapCreate(
                tap: .cgSessionEventTap,
                place: .headInsertEventTap,
                options: .listenOnly,
                eventsOfInterest: mask,
                callback: callback,
                userInfo: Unmanaged.passUnretained(self).toOpaque()
            ) else {
                startErrorBox.error = .eventTapCreationFailed
                ready.signal()
                return
            }

            let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
            self.tap = tap
            self.runLoopSource = source
            self.tapRunLoop = CFRunLoopGetCurrent()
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source, .commonModes)
            CGEvent.tapEnable(tap: tap, enable: true)
            ready.signal()
            CFRunLoopRun()
        }
        t.name = "LFCapture.EventTap"
        t.qualityOfService = .userInteractive
        thread = t
        t.start()

        ready.wait()
        if let error = startErrorBox.error {
            thread = nil
            throw error
        }
    }

    func stop() {
        if let tap { CGEvent.tapEnable(tap: tap, enable: false) }
        if let runLoopSource { CFRunLoopSourceInvalidate(runLoopSource) }
        if let tap { CFMachPortInvalidate(tap) }
        if let tapRunLoop { CFRunLoopStop(tapRunLoop) }
        tap = nil
        runLoopSource = nil
        tapRunLoop = nil
        thread = nil
        activeKey = nil
    }

    // MARK: - Event handling (tap thread)

    private func handle(type: CGEventType, event: CGEvent) {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            // macOS silently disables taps that stall (or on secure input);
            // re-enable immediately or the hotkey goes dead.
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }

        case .flagsChanged:
            let code = event.getIntegerValueField(.keyboardEventKeycode)
            for key in keys where key.usesFlagsChanged && key.virtualKeyCode == code {
                let held = key.isHeld(in: event.flags)
                if held { triggerDown(key) } else { triggerUp(key) }
            }

        case .keyDown:
            let code = event.getIntegerValueField(.keyboardEventKeycode)
            if code == 53 { // Esc
                onEscape?()
                return
            }
            guard event.getIntegerValueField(.keyboardEventAutorepeat) == 0 else { return }
            for key in keys {
                if case .keyCode(let k) = key, code == k { triggerDown(key) }
            }

        case .keyUp:
            let code = event.getIntegerValueField(.keyboardEventKeycode)
            for key in keys {
                if case .keyCode(let k) = key, code == k { triggerUp(key) }
            }

        case .otherMouseDown:
            let button = event.getIntegerValueField(.mouseEventButtonNumber)
            for key in keys {
                if case .mouseButton(let b) = key, button == b { triggerDown(key) }
            }

        case .otherMouseUp:
            let button = event.getIntegerValueField(.mouseEventButtonNumber)
            for key in keys {
                if case .mouseButton(let b) = key, button == b { triggerUp(key) }
            }

        default:
            break
        }
    }

    private func triggerDown(_ key: HotkeyKey) {
        guard activeKey == nil else { return }
        activeKey = key
        onKeyDown?()
    }

    private func triggerUp(_ key: HotkeyKey) {
        guard activeKey == key else { return }
        activeKey = nil
        onKeyUp?()
    }
}
