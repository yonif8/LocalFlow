import CoreAudio
import os

/// Ducks the default audio output device to 20% volume while push-to-talk is
/// recording, so music or video playing on the speakers doesn't drown out the
/// microphone. Pure CoreAudio HAL — no permissions, works for any player
/// (Music, Spotify, browser tabs, …), and HAL property writes never trigger
/// macOS's on-screen volume bezel (that's tied to the volume keys / Control
/// Center, not the device property).
///
/// duck() remembers exactly what it changed (device + element + prior value)
/// and restore() puts it back. Channels already at or below the duck level
/// are left alone (never raise the volume). Devices with no settable volume
/// control at all (some digital/AirPlay outputs) fall back to the master
/// mute switch.
@MainActor
final class SystemAudioDucker {
    static let shared = SystemAudioDucker()
    private static let logger = Logger(subsystem: "com.localflow.app", category: "audioduck")

    /// Absolute output level while dictating.
    private static let duckLevel: Float32 = 0.20

    private enum Applied {
        case volume(device: AudioDeviceID, element: AudioObjectPropertyElement, previous: Float32)
        case mute(device: AudioDeviceID)
    }
    /// What duck() changed; empty when we haven't touched the system.
    private var applied: [Applied] = []

    private init() {}

    func duck() {
        guard applied.isEmpty else { return }
        guard let device = Self.defaultOutputDevice() else {
            Self.logger.info("no default output device; skipping duck")
            return
        }

        // Master volume element when the device has one; otherwise
        // per-channel (stereo devices expose 1 and 2).
        let main = kAudioObjectPropertyElementMain
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
                    previous > Self.duckLevel,
                    Self.writeFloat32(
                        Self.duckLevel, to: device,
                        selector: kAudioDevicePropertyVolumeScalar, element: element)
                else { continue }
                applied.append(.volume(device: device, element: element, previous: previous))
            }
            // Already quiet enough → applied stays empty, nothing to restore.
            if !applied.isEmpty {
                Self.logger.info("output ducked to \(Self.duckLevel, privacy: .public) (device \(device, privacy: .public))")
            }
            return
        }

        // No volume control at all — fall back to the mute switch so loud
        // playback still can't drown the mic.
        if Self.isSettable(device, selector: kAudioDevicePropertyMute, element: main),
           Self.readUInt32(device, selector: kAudioDevicePropertyMute, element: main) == 0,
           Self.writeUInt32(1, to: device, selector: kAudioDevicePropertyMute, element: main) {
            applied = [.mute(device: device)]
            Self.logger.info("no volume control; output muted instead (device \(device, privacy: .public))")
        } else {
            Self.logger.info("output device \(device, privacy: .public) has no settable volume/mute; skipping")
        }
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
