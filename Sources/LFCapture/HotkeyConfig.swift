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

    /// True for keys that only produce `.flagsChanged` events (modifiers).
    var usesFlagsChanged: Bool {
        switch self {
        case .fn, .rightCommand, .rightOption: return true
        case .keyCode: return false
        }
    }

    /// Virtual keycode reported in the CGEvent's `.keyboardEventKeycode` field.
    var virtualKeyCode: Int64 {
        switch self {
        case .fn: return 63            // kVK_Function
        case .rightCommand: return 54  // kVK_RightCommand
        case .rightOption: return 61   // kVK_RightOption
        case .keyCode(let k): return k
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
        case .keyCode:
            return false
        }
    }

    public var description: String {
        switch self {
        case .fn: return "Fn (Globe)"
        case .rightCommand: return "Right Command"
        case .rightOption: return "Right Option"
        case .keyCode(let k): return "keycode \(k)"
        }
    }
}

/// Configuration for the hold-to-talk capture engine.
public struct HotkeyConfig: Sendable {
    /// The hold-to-talk key.
    public var key: HotkeyKey
    /// Holds shorter than this are treated as accidental taps and cancelled (Hex uses 0.3s).
    public var holdThreshold: TimeInterval
    /// Utterances shorter than this are cancelled even if the hold was long enough.
    public var minUtteranceDuration: TimeInterval
    /// Keep AVAudioEngine running with a discarding tap between utterances.
    /// Mitigates the 1-2s mic spin-up on AirPods/Bluetooth inputs and avoids
    /// losing the first syllable. Costs an always-on mic stream (samples are
    /// discarded while idle, never stored).
    public var keepMicWarm: Bool

    public init(
        key: HotkeyKey = .fn,
        holdThreshold: TimeInterval = 0.3,
        minUtteranceDuration: TimeInterval = 0.3,
        keepMicWarm: Bool = true
    ) {
        self.key = key
        self.holdThreshold = holdThreshold
        self.minUtteranceDuration = minUtteranceDuration
        self.keepMicWarm = keepMicWarm
    }
}
