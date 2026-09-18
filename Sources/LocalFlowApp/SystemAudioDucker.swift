import CoreAudio
import os

/// Ducks the default audio output device to a user-chosen level (default
/// 20%, 0 = mute) while push-to-talk is recording, so music or video playing
/// on the speakers doesn't drown out the microphone. Pure CoreAudio HAL — no
/// permissions, works for any player (Music, Spotify, browser tabs, …), and
/// HAL property writes never trigger macOS's on-screen volume bezel (that's
/// tied to the volume keys / Control Center, not the device property).
///
/// duck(to:) remembers exactly what it changed (device + element + prior
/// value) and restore() puts it back. Channels already at or below the duck
/// level are left alone (never raise the volume). A level of 0 uses the
/// master mute switch when available (a volume scalar of 0 is not silent on
/// every device); devices with no settable volume control at all (some
/// digital/AirPlay outputs) also fall back to the mute switch.
@MainActor
final class SystemAudioDucker {
    static let shared = SystemAudioDucker()
    private static let logger = Logger(subsystem: "com.localflow.app", category: "audioduck")

    private enum Applied {
        case volume(device: AudioDeviceID, element: AudioObjectPropertyElement, previous: Float32)
        case mute(device: AudioDeviceID)
    }
    /// What duck(to:) changed; empty when we haven't touched the system.
    private var applied: [Applied] = []

    private init() {}

    /// - Parameter level: absolute output volume scalar while dictating,
    ///   clamped to 0...1. 0 mutes.
    func duck(to level: Float32) {
        guard applied.isEmpty else { return }
        let level = min(max(level, 0), 1)
        guard let device = Self.defaultOutputDevice() else {
            Self.logger.info("no default output device; skipping duck")
            return
        }
        let main = kAudioObjectPropertyElementMain

        if level == 0, Self.isSettable(device, selector: kAudioDevicePropertyMute, element: main) {
            muteIfUnmuted(device)
            return
        }

        // Master volume element when the device has one; otherwise
        // per-channel (stereo devices expose 1 and 2).
        let candidates: [AudioObjectPropertyElement] =
            Self.isSettable(device, selector: kAudioDevicePropertyVolumeScalar, element: main)
            ? [main] : [1, 2]
        let elements = candidates.filter {
            Self.isSettable(device, selector: kAudioDevicePropertyVolumeScalar, element: $0)
        }

        if !elements.isEmpty {
            for element in elements {
                guard
                    let previous = Self.readFloat32(
                        device, selector: kAudioDevicePropertyVolumeScalar, element: element),
                    previous > level,
                    Self.writeFloat32(
                        level, to: device,
                        selector: kAudioDevicePropertyVolumeScalar, element: element)
                else { continue }
                applied.append(.volume(device: device, element: element, previous: previous))
            }
            // Already quiet enough → applied stays empty, nothing to restore.
            if !applied.isEmpty {
                Self.logger.info("output ducked to \(level, privacy: .public) (device \(device, privacy: .public))")
            }
            return
        }

        // No volume control at all — fall back to the mute switch so loud
        // playback still can't drown the mic.
        if Self.isSettable(device, selector: kAudioDevicePropertyMute, element: main) {
            muteIfUnmuted(device)
        } else {
            Self.logger.info("output device \(device, privacy: .public) has no settable volume/mute; skipping")
        }
    }

    /// Mutes via the master mute switch unless the user already muted it
    /// (in which case restore() must leave it muted).
    private func muteIfUnmuted(_ device: AudioDeviceID) {
        let main = kAudioObjectPropertyElementMain
        guard Self.readUInt32(device, selector: kAudioDevicePropertyMute, element: main) == 0,
              Self.writeUInt32(1, to: device, selector: kAudioDevicePropertyMute, element: main)
        else { return }
        applied = [.mute(device: device)]
        Self.logger.info("output muted while dictating (device \(device, privacy: .public))")
    }

    func restore() {
        guard !applied.isEmpty else { return }
        for action in applied {
            switch action {
            case .volume(let device, let element, let previous):
                _ = Self.writeFloat32(
                    previous, to: device,
                    selector: kAudioDevicePropertyVolumeScalar,
                    element: element)
            case .mute(let device):
                _ = Self.writeUInt32(
                    0, to: device,
                    selector: kAudioDevicePropertyMute,
                    element: kAudioObjectPropertyElementMain)
            }
        }
        applied.removeAll()
        Self.logger.info("output audio restored")
    }

    // MARK: - CoreAudio plumbing

    private static func outputAddress(
        _ selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: element)
    }

    private static func defaultOutputDevice() -> AudioDeviceID? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var device = AudioDeviceID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device)
        guard status == noErr, device != kAudioObjectUnknown else { return nil }
        return device
    }

    private static func isSettable(
        _ device: AudioDeviceID,
        selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> Bool {
        var address = outputAddress(selector, element: element)
        guard AudioObjectHasProperty(device, &address) else { return false }
        var settable = DarwinBoolean(false)
        return AudioObjectIsPropertySettable(device, &address, &settable) == noErr
            && settable.boolValue
    }

    private static func readUInt32(
        _ device: AudioDeviceID,
        selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> UInt32? {
        var address = outputAddress(selector, element: element)
        var value = UInt32(0)
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr
        else { return nil }
        return value
    }

    private static func writeUInt32(
        _ value: UInt32,
        to device: AudioDeviceID,
        selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> Bool {
        var address = outputAddress(selector, element: element)
        var value = value
        let size = UInt32(MemoryLayout<UInt32>.size)
        return AudioObjectSetPropertyData(device, &address, 0, nil, size, &value) == noErr
    }

    private static func readFloat32(
        _ device: AudioDeviceID,
        selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> Float32? {
        var address = outputAddress(selector, element: element)
        var value = Float32(0)
        var size = UInt32(MemoryLayout<Float32>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &value) == noErr
        else { return nil }
        return value
    }

    private static func writeFloat32(
        _ value: Float32,
        to device: AudioDeviceID,
        selector: AudioObjectPropertySelector,
        element: AudioObjectPropertyElement
    ) -> Bool {
        var address = outputAddress(selector, element: element)
        var value = value
        let size = UInt32(MemoryLayout<Float32>.size)
        return AudioObjectSetPropertyData(device, &address, 0, nil, size, &value) == noErr
    }
}
