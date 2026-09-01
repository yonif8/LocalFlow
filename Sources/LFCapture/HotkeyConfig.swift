import CoreGraphics
import Foundation

/// Which physical key arms hold-to-talk.
public enum HotkeyKey: Sendable, Equatable, CustomStringConvertible {
    /// Fn / Globe key (default). Detected via `.flagsChanged` + `maskSecondaryFn`.
    case fn
    /// Right Command key. Detected via `.flagsChanged` + the device-specific right-cmd bit.
    case rightCommand
    /// Right Option key. Detected via `.flagsChanged` + the device-specific right-alt bit.
    case rightOption
    /// Any non-modifier key by virtual keycode (e.g. 96 = F5). Detected via keyDown/keyUp.
    case keyCode(Int64)
    /// A mouse button by CGEvent button number (2 = middle, 3+ = side buttons).
    /// Detected via otherMouseDown/otherMouseUp; left (0) and right (1) are
    /// deliberately unsupported — hijacking them would break normal clicking.
    case mouseButton(Int64)

    /// True for keys that only produce `.flagsChanged` events (modifiers).
    var usesFlagsChanged: Bool {
        switch self {
        case .fn, .rightCommand, .rightOption: return true
        case .keyCode, .mouseButton: return false
        }
    }

    /// Virtual keycode reported in the CGEvent's `.keyboardEventKeycode` field.
    var virtualKeyCode: Int64 {
        switch self {
        case .fn: return 63            // kVK_Function
        case .rightCommand: return 54  // kVK_RightCommand
        case .rightOption: return 61   // kVK_RightOption
        case .keyCode(let k): return k
        case .mouseButton: return -1   // not a keyboard key
        }
    }

    /// For modifier keys: is the key currently held, given the event's flags?
    /// Uses device-specific NX_DEVICE* bits for right-side modifiers so a held
    /// left-side sibling doesn't confuse up/down detection.
    func isHeld(in flags: CGEventFlags) -> Bool {
        switch self {
        case .fn:
            return flags.contains(.maskSecondaryFn)
        case .rightCommand:
            return flags.rawValue & 0x0000_0010 != 0 // NX_DEVICERCMDKEYMASK
        case .rightOption:
            return flags.rawValue & 0x0000_0040 != 0 // NX_DEVICERALTKEYMASK
        case .keyCode, .mouseButton:
            return false
        }
    }

    public var description: String {
        switch self {
        case .fn: return "Fn (Globe)"
        case .rightCommand: return "Right Command"
        case .rightOption: return "Right Option"
        case .keyCode(let k): return "keycode \(k)"
        case .mouseButton(let b): return "Mouse Button \(b + 1)"
        }
    }
}

/// Configuration for the hold-to-talk capture engine.
public struct HotkeyConfig: Sendable {
    /// The hold-to-talk key.
    public var key: HotkeyKey
    /// Optional second trigger (e.g. a spare mouse button). Either trigger
    /// arms recording; release of the one that armed it ends the utterance.
    public var secondaryKey: HotkeyKey?
    /// Holds shorter than this are treated as accidental taps and cancelled (Hex uses 0.3s).
    public var holdThreshold: TimeInterval
    /// Utterances shorter than this are cancelled even if the hold was long enough.
    public var minUtteranceDuration: TimeInterval
    /// CoreAudio device UID of the microphone to record from; nil = system
    /// default. If the device is missing at start, capture falls back to the
    /// default rather than failing.
    public var microphoneUID: String?
    /// Keep AVAudioEngine running with a discarding tap between utterances.
    /// Mitigates the 1-2s mic spin-up on AirPods/Bluetooth inputs and avoids
    /// losing the first syllable. Costs an always-on mic stream (samples are
    /// discarded while idle, never stored).
    public var keepMicWarm: Bool

    public init(
        key: HotkeyKey = .fn,
        secondaryKey: HotkeyKey? = nil,
        microphoneUID: String? = nil,
        holdThreshold: TimeInterval = 0.3,
        minUtteranceDuration: TimeInterval = 0.3,
        keepMicWarm: Bool = true
    ) {
        self.key = key
        self.secondaryKey = secondaryKey
        self.microphoneUID = microphoneUID
        self.holdThreshold = holdThreshold
        self.minUtteranceDuration = minUtteranceDuration
        self.keepMicWarm = keepMicWarm
    }
}
