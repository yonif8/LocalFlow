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
    private let key: HotkeyKey

    /// Callbacks fire on the tap thread.
    var onKeyDown: (() -> Void)?
    var onKeyUp: (() -> Void)?
    var onEscape: (() -> Void)?

    private var tap: CFMachPort?
    private var runLoopSource: CFRunLoopSource?
    private var tapRunLoop: CFRunLoop?
    private var thread: Thread?
    private var keyIsDown = false

    init(key: HotkeyKey) {
        self.key = key
    }

    func start() throws {
        guard thread == nil else { return }

        let ready = DispatchSemaphore(value: 0)
        // Written on the tap thread before `ready.signal()`, read after `ready.wait()`.
        let startErrorBox = ErrorBox()

        let t = Thread { [weak self] in
            guard let self else { ready.signal(); return }

            let mask: CGEventMask =
                (CGEventMask(1) << CGEventType.flagsChanged.rawValue)
                | (CGEventMask(1) << CGEventType.keyDown.rawValue)
                | (CGEventMask(1) << CGEventType.keyUp.rawValue)

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
        keyIsDown = false
    }

    // MARK: - Event handling (tap thread)

    private func handle(type: CGEventType, event: CGEvent) {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            // macOS silently disables taps that stall (or on secure input);
            // re-enable immediately or the hotkey goes dead.
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }

        case .flagsChanged:
            guard key.usesFlagsChanged else { return }
            let code = event.getIntegerValueField(.keyboardEventKeycode)
            guard code == key.virtualKeyCode else { return }
            let held = key.isHeld(in: event.flags)
            if held != keyIsDown {
                keyIsDown = held
                held ? onKeyDown?() : onKeyUp?()
            }

        case .keyDown:
            let code = event.getIntegerValueField(.keyboardEventKeycode)
            if code == 53 { // Esc
                onEscape?()
                return
            }
            if case .keyCode(let k) = key, code == k,
               event.getIntegerValueField(.keyboardEventAutorepeat) == 0,
               !keyIsDown {
                keyIsDown = true
                onKeyDown?()
            }

        case .keyUp:
            if case .keyCode(let k) = key,
               event.getIntegerValueField(.keyboardEventKeycode) == k,
               keyIsDown {
                keyIsDown = false
                onKeyUp?()
            }

        default:
            break
        }
    }
}
