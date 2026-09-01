import AVFoundation
import CoreGraphics
import Foundation

/// Permission checks/requests for the capture pipeline.
///
/// A LISTEN-ONLY event tap needs Input Monitoring (not Accessibility);
/// the microphone needs the usual AV capture grant.
public enum CapturePermissions {
    /// Non-prompting check for Input Monitoring (listen-event access).
    public static func inputMonitoringGranted() -> Bool {
        CGPreflightListenEventAccess()
    }

    /// Prompts the user (adds this process's responsible app to the
    /// Input Monitoring pane). Returns current grant state; a fresh grant
    /// usually requires relaunching the process.
    @discardableResult
    public static func requestInputMonitoring() -> Bool {
        CGRequestListenEventAccess()
    }

    public static func microphoneStatus() -> AVAuthorizationStatus {
        AVCaptureDevice.authorizationStatus(for: .audio)
    }

    public static func requestMicrophone() async -> Bool {
        await AVCaptureDevice.requestAccess(for: .audio)
    }
}

/// Errors thrown by `HoldToTalkCaptureEngine.start()`.
public enum CaptureError: Error, CustomStringConvertible {
    case inputMonitoringDenied
    case microphoneDenied
    case eventTapCreationFailed
    case audioEngineFailed(String)
    case noInputDevice
    /// The configured microphone (by UID) is not connected/usable.
    case microphoneUnavailable(String)

    public var description: String {
        switch self {
        case .inputMonitoringDenied:
            return "Input Monitoring permission is missing. Grant it in System Settings > Privacy & Security > Input Monitoring, then relaunch."
        case .microphoneDenied:
            return "Microphone permission is denied. Grant it in System Settings > Privacy & Security > Microphone, then relaunch."
        case .eventTapCreationFailed:
            return "Could not create the keyboard event tap (usually missing Input Monitoring permission)."
        case .audioEngineFailed(let why):
            return "Audio engine failed to start: \(why)"
        case .noInputDevice:
            return "No audio input device available (input format reported 0 Hz)."
        case .microphoneUnavailable(let uid):
            return "The selected microphone (\(uid)) is not connected; falling back to the system default."
        }
    }
}
