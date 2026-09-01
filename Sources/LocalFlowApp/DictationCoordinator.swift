import Foundation
import AppKit
import Observation
import os
import LFContracts
import LFCapture
import LFInsert
import LFPolish

struct Transcript: Identifiable, Sendable {
    let id = UUID()
    let text: String
    let date: Date

    var menuTitle: String {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.count > 60 ? String(trimmed.prefix(57)) + "…" : trimmed
    }
}

/// Owns the dictation pipeline:
/// CaptureEngine.events → .ended → Transcriber.transcribe → TextPolisher.polish
/// → TextInserter.insert, with HUD state transitions and error surfacing.
///
/// Real components wired: LFEngine.GraniteTranscriber,
/// LFCapture.HoldToTalkCaptureEngine (falls back to mock-only if permissions
/// are missing), LFPolish.LocalPolisher, LFInsert.FrontmostInserter (falls
/// back to pasteboard copy if Accessibility is missing).
@MainActor
@Observable
final class DictationCoordinator {
    enum State: Equatable {
        case idle
        case recording
        case processing
        case error(String)
    }

    static let shared = DictationCoordinator()
    private static let logger = Logger(subsystem: "com.localflow.app", category: "coordinator")

    private(set) var state: State = .idle
    /// Live input level 0...1 for the HUD meter.
    private(set) var level: Float = 0
    private(set) var isListening = false
    private(set) var history: [Transcript] = []
    /// Why real hotkey capture isn't running (shown in the menu), nil if it is.
    private(set) var captureLimitation: String?

    // Pipeline components — protocol-typed so the orchestrator can rewire.
    private let mockCapture = MockCaptureEngine()
    private var realCapture: HoldToTalkCaptureEngine?
    private let transcriber: any Transcriber
    private let polisher: any TextPolisher
    private let inserter: any TextInserter

    private var pumpTasks: [Task<Void, Never>] = []
    private var errorResetTask: Task<Void, Never>?
    private var didPrepareTranscriber = false

    private init() {
        self.transcriber = EngineFactory.makeTranscriber()
        self.polisher = Self.makePolisher()
        self.inserter = AdaptiveInserter()
    }

    private static func makePolisher() -> any TextPolisher {
        let dictionaryURL = URL(fileURLWithPath: NSString(
            string: "~/Library/Application Support/LocalFlow/dictionary.json"
        ).expandingTildeInPath)
        let dictionary = (try? PersonalDictionary.load(from: dictionaryURL))
            ?? PersonalDictionary()
        return LocalPolisher(dictionary: dictionary)
    }

    var menuBarSymbolName: String {
        switch state {
        // Idle icon is deliberately not a mic: macOS shows its own orange
        // mic pill while recording, and two mic glyphs side by side is noise.
        case .idle: return isListening ? "waveform" : "waveform.slash"
        case .recording: return "mic.fill"
        case .processing: return "waveform"
        case .error: return "mic.badge.xmark"
        }
    }

    func startListening() {
        guard !isListening else { return }

        // Warm the transcriber once so the first real utterance isn't slow
        // (~2.1 s cold vs ~110 ms warm for the Granite engine).
        if !didPrepareTranscriber {
            didPrepareTranscriber = true
            if let preparable = transcriber as? PreparableTranscriber {
                Task { await preparable.prepare() }
            }
            // Same idea for the polish model: a cold first call would blow
            // the polish timeout and leave a zombie request competing with
            // the ASR engine for the GPU/ANE.
            if let localPolisher = polisher as? LocalPolisher {
                Task.detached(priority: .utility) { localPolisher.prewarm() }
            }
        }

        // Mock capture always runs so "Simulate Dictation" works everywhere.
        try? mockCapture.start()
        pump(mockCapture.events)

        // Real hold-to-talk capture: needs Input Monitoring + Microphone.
        // start() throws when permissions are missing; fall back gracefully.
        // keepMicWarm: false — holding the mic open between utterances keeps
        // macOS's orange mic-in-use indicator on permanently, which reads as
        // a second mic icon in the menu bar. Built-in mic spin-up is fast;
        // revisit for Bluetooth mics via a settings toggle if needed.
        let storedButton = UserDefaults.standard.object(forKey: DefaultsKey.mouseButton) as? Int
        let secondary: HotkeyKey? = storedButton.flatMap { $0 >= 2 ? .mouseButton(Int64($0)) : nil }
        let engine = HoldToTalkCaptureEngine(
            config: HotkeyConfig(
                key: HotkeyChoice.load().captureKey,
                secondaryKey: secondary,
                keepMicWarm: false
            )
        )
        do {
            try engine.start()
            realCapture = engine
            captureLimitation = nil
            pump(engine.events)
        } catch {
            realCapture = nil
            captureLimitation = "Hotkey capture off: \(String(describing: error))"
            Self.logger.info("real capture unavailable: \(String(describing: error), privacy: .public)")
        }

        isListening = true
        Self.logger.info("listening started (real capture: \(self.realCapture != nil, privacy: .public))")
    }

    func stopListening() {
        realCapture?.stop()
        realCapture = nil
        mockCapture.stop()
        pumpTasks.forEach { $0.cancel() }
        pumpTasks.removeAll()
        isListening = false
        captureLimitation = nil
        state = .idle
        HUDController.shared.hide()
    }

    /// Restart capture so a changed hotkey setting takes effect.
    func restartListeningIfNeeded() {
        guard isListening else { return }
        stopListening()
        startListening()
    }

    /// Debug: drive a full mock capture session (began → levels → ended).
    func simulateDictation() {
        Self.logger.info("simulateDictation requested")
        mockCapture.simulateDictation(utterance: EngineFactory.simulationUtterance())
    }

    func copyToClipboard(_ transcript: Transcript) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(transcript.text, forType: .string)
    }

    private func pump(_ events: AsyncStream<CaptureEvent>) {
        let task = Task { [weak self] in
            for await event in events {
                guard let self, !Task.isCancelled else { break }
                await self.handle(event)
            }
        }
        pumpTasks.append(task)
    }

    private func handle(_ event: CaptureEvent) async {
        switch event {
        case .began:
            Self.logger.info("capture began")
            errorResetTask?.cancel()
            state = .recording
            level = 0
            HUDController.shared.show()

        case .level(let value):
            level = max(0, min(1, value))

        case .cancelled:
            state = .idle
            level = 0
            HUDController.shared.hide()

        case .ended(let utterance):
            Self.logger.info("capture ended (\(utterance.duration, privacy: .public)s); transcribing")
            state = .processing
            level = 0
            await runPipeline(utterance)
        }
    }

    private func runPipeline(_ utterance: Utterance) async {
        // A warm engine finishes in ~110 ms; keep the "processing…" lozenge up
        // for a beat so it reads as a state, not a flicker. (Cold runs take
        // seconds and are unaffected.)
        let processingShownAt = ContinuousClock.now
        do {
            let raw = try await transcriber.transcribe(utterance)
            var text = raw
            if UserDefaults.standard.object(forKey: DefaultsKey.polishEnabled) as? Bool ?? true {
                let context = PolishContext(
                    targetAppBundleID: NSWorkspace.shared.frontmostApplication?.bundleIdentifier
                )
                text = await polisher.polish(text, context: context)
            }
            try await inserter.insert(text)
            appendHistory(text)
            Self.logger.info("pipeline complete: \"\(text, privacy: .public)\"")
            let elapsed = ContinuousClock.now - processingShownAt
            if elapsed < .milliseconds(350) {
                try? await Task.sleep(for: .milliseconds(350) - elapsed)
            }
            state = .idle
            HUDController.shared.hide()
        } catch {
            Self.logger.error("pipeline failed: \(String(describing: error), privacy: .public)")
            surfaceError("Dictation failed: \(error.localizedDescription)")
        }
    }

    private func appendHistory(_ text: String) {
        history.insert(Transcript(text: text, date: Date()), at: 0)
        if history.count > 10 {
            history.removeLast(history.count - 10)
        }
    }

    /// Error surfacing: HUD flashes the message, then everything resets.
    private func surfaceError(_ message: String) {
        state = .error(message)
        HUDController.shared.show()
        errorResetTask?.cancel()
        errorResetTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2.5))
            guard let self, !Task.isCancelled else { return }
            if case .error = self.state {
                self.state = .idle
                HUDController.shared.hide()
            }
        }
    }
}

/// Tries the real LFInsert inserter (AX/paste at the caret); if that fails
/// (e.g. Accessibility not granted), falls back to copying to the pasteboard
/// so the user still gets the text.
struct AdaptiveInserter: TextInserter {
    private let primary = FrontmostInserter()
    private let fallback = PasteboardInserter()

    func insert(_ text: String) async throws {
        do {
            try await primary.insert(text)
        } catch {
            try await fallback.insert(text)
        }
    }
}

extension HotkeyChoice {
    /// Bridge to LFCapture's public key type.
    var captureKey: HotkeyKey {
        switch self {
        case .fn: return .fn
        case .rightCommand: return .rightCommand
        case .rightOption: return .rightOption
        }
    }
}
