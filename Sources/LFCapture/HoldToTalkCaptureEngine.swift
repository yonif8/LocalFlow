import AVFoundation
import Foundation
import LFContracts

/// Hold-to-talk `CaptureEngine`:
///
/// - listen-only CGEventTap detects the hotkey (default: hold Fn/Globe)
/// - recording starts immediately on key-down (`.began`)
/// - key-up ends the utterance (`.ended`) — unless the hold was shorter than
///   `holdThreshold` or the audio shorter than `minUtteranceDuration`, which
///   yields `.cancelled` (accidental-tap rejection)
/// - Esc while recording cancels (`.cancelled`)
/// - `.level` events (0...1) are emitted while recording for HUD metering
public final class HoldToTalkCaptureEngine: CaptureEngine, @unchecked Sendable {
    public let events: AsyncStream<CaptureEvent>

    private let continuation: AsyncStream<CaptureEvent>.Continuation
    private let config: HotkeyConfig
    private let monitor: EventTapHotkeyMonitor
    private let recorder: MicRecorder
    private let lock = NSLock()
    private var recordingStartedAt: Date?
    private var started = false

    public init(config: HotkeyConfig = HotkeyConfig()) {
        self.config = config
        self.monitor = EventTapHotkeyMonitor(
            keys: [config.key] + (config.secondaryKey.map { [$0] } ?? [])
        )
        self.recorder = MicRecorder()
        (events, continuation) = AsyncStream.makeStream(of: CaptureEvent.self)

        monitor.onKeyDown = { [weak self] in self?.hotkeyDown() }
        monitor.onKeyUp = { [weak self] in self?.hotkeyUp() }
        monitor.onEscape = { [weak self] in self?.escapePressed() }
        recorder.onLevel = { [weak self] level in
            guard let self else { return }
            self.lock.lock()
            let active = self.recordingStartedAt != nil
            self.lock.unlock()
            if active { self.continuation.yield(.level(level)) }
        }
    }

    /// Install event tap + prepare audio. Throws if permissions are missing.
    public func start() throws {
        lock.lock()
        let alreadyStarted = started
        lock.unlock()
        guard !alreadyStarted else { return }

        if CapturePermissions.microphoneStatus() == .denied {
            throw CaptureError.microphoneDenied
        }
        guard CapturePermissions.inputMonitoringGranted() else {
            throw CaptureError.inputMonitoringDenied
        }

        do {
            try recorder.prepare(keepWarm: config.keepMicWarm, microphoneUID: config.microphoneUID)
        } catch CaptureError.microphoneUnavailable {
            // Selected mic unplugged: degrade to the system default.
            try recorder.prepare(keepWarm: config.keepMicWarm, microphoneUID: nil)
        }
        try monitor.start()

        lock.lock()
        started = true
        lock.unlock()
    }

    public func stop() {
        lock.lock()
        let wasStarted = started
        started = false
        recordingStartedAt = nil
        lock.unlock()
        guard wasStarted else { return }
        monitor.stop()
        recorder.shutdown()
        continuation.finish()
    }

    // MARK: - Hotkey state machine (tap thread)

    private func hotkeyDown() {
        lock.lock()
        guard started, recordingStartedAt == nil else {
            lock.unlock()
            return
        }
        recordingStartedAt = Date()
        lock.unlock()

        do {
            try recorder.beginRecording()
            continuation.yield(.began)
        } catch {
            lock.lock()
            recordingStartedAt = nil
            lock.unlock()
            continuation.yield(.cancelled)
        }
    }

    private func hotkeyUp() {
        lock.lock()
        guard let startedAt = recordingStartedAt else {
            lock.unlock()
            return
        }
        recordingStartedAt = nil
        lock.unlock()

        let heldFor = Date().timeIntervalSince(startedAt)
        let samples = recorder.endRecording()

        guard heldFor >= config.holdThreshold else {
            continuation.yield(.cancelled) // accidental tap
            return
        }
        let utterance = Utterance(samples: samples, sampleRate: MicRecorder.targetSampleRate)
        guard utterance.duration >= config.minUtteranceDuration else {
            continuation.yield(.cancelled) // too short to transcribe
            return
        }
        continuation.yield(.ended(utterance))
    }

    private func escapePressed() {
        lock.lock()
        guard recordingStartedAt != nil else {
            lock.unlock()
            return
        }
        recordingStartedAt = nil
        lock.unlock()

        recorder.cancelRecording()
        continuation.yield(.cancelled)
    }
}
