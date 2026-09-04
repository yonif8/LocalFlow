import Foundation
import AppKit
import Observation
import os
import LFContracts
import LFCapture
import LFEngine
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
/// Real components wired: LFEngine.ParakeetTranscriber,
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
    // Polisher and inserter are rebuilt from AppSettings via applySettings();
    // the S1 model itself is a process-wide singleton, so rebuilds are cheap.
    private let mockCapture = MockCaptureEngine()
    private var realCapture: HoldToTalkCaptureEngine?
    private var transcriber: any Transcriber
    private var polisher: any TextPolisher
    private var inserter: any TextInserter

    private var pumpTasks: [Task<Void, Never>] = []
    private var lastLevelLog = ContinuousClock.now
    private var levelLogPeak: Float = 0
    private var pendingScreenContext: ScreenContextSnapshot?
    private var errorResetTask: Task<Void, Never>?
    private var didPrepareTranscriber = false

    private init() {
        // One-time move of the legacy model caches (~/Documents/huggingface,
        // ~/.cache/huggingface/hub) into Application Support — must run
        // before anything can trigger a model load.
        ModelLocations.migrateLegacyCachesIfNeeded()
        // S1-mini download progress → onboarding "Models" section.
        PolishModelStore.progressHandler = { progress in
            Task { @MainActor in
                ModelSetupState.shared.notePolishProgress(progress)
            }
        }
        self.transcriber = EngineFactory.makeTranscriber()
        self.polisher = Self.makePolisher()
        self.inserter = AdaptiveInserter(configuration: AppSettings.inserterConfiguration)
    }

    private static func makePolisher() -> any TextPolisher {
        LocalPolisher(
            dictionary: AppSettings.loadDictionary(),
            configuration: .init(
                llmEnabled: AppSettings.polishEnabled,
                timeout: AppSettings.polishTimeout,
                maxInputCharacters: AppSettings.polishMaxChars,
                toneOverride: AppSettings.polishToneOverride
            )
        )
    }

    /// Re-read AppSettings and rebuild the affected pipeline pieces.
    /// Settings UI calls this after any change; capture-related changes
    /// additionally restart listening (the event tap holds its config).
    func applySettings(restartCapture: Bool = false) {
        polisher = Self.makePolisher()
        inserter = AdaptiveInserter(configuration: AppSettings.inserterConfiguration)
        trimHistory()
        if restartCapture {
            restartListeningIfNeeded()
        }
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

        // First run with models missing: surface the onboarding window so
        // the user sees download progress instead of a silent multi-minute
        // stall. (App.swift already shows it for missing permissions.)
        ModelSetupState.shared.refreshFromDisk()
        if !ModelSetupState.shared.allDownloaded {
            OnboardingWindowController.shared.show()
        }

        // Warm the transcriber once so the first real utterance isn't slow
        // (model load + first-ever ~600 MB download happen here, not mid-dictation).
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
        let engine = HoldToTalkCaptureEngine(
            config: HotkeyConfig(
                key: HotkeyChoice.load().captureKey,
                secondaryKey: AppSettings.mouseButton.map { .mouseButton(Int64($0)) },
                microphoneUID: AppSettings.microphoneUID,
                holdThreshold: AppSettings.holdThreshold,
                // Note: keeping the mic warm leaves macOS's orange mic
                // indicator on permanently — surfaced in Settings.
                keepMicWarm: AppSettings.keepMicWarm
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
        SystemAudioDucker.shared.restore()
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
            if AppSettings.screenTerminologyEnabled {
                let snapshot = ScreenContextCollector.capture()
                pendingScreenContext = snapshot
                Self.logger.info("""
                    screen context: \(snapshot.terms.count, privacy: .public) terms from \
                    \(snapshot.visitedElements, privacy: .public) elements in \
                    \(String(describing: snapshot.elapsed), privacy: .public)
                    """)
            } else {
                pendingScreenContext = nil
            }
            state = .recording
            level = 0
            if AppSettings.duckWhileDictating {
                SystemAudioDucker.shared.duck()
            }
            HUDController.shared.show()

        case .level(let value):
            level = max(0, min(1, value))
            // Throttled diagnostics for "meter not moving" reports: peak
            // level once per second while recording.
            levelLogPeak = max(levelLogPeak, level)
            if ContinuousClock.now - lastLevelLog > .seconds(1) {
                Self.logger.info("level peak: \(String(format: "%.2f", self.levelLogPeak), privacy: .public)")
                lastLevelLog = .now
                levelLogPeak = 0
            }

        case .cancelled:
            SystemAudioDucker.shared.restore()
            state = .idle
            level = 0
            HUDController.shared.hide()
            pendingScreenContext = nil

        case .ended(let utterance):
            Self.logger.info("capture ended (\(utterance.duration, privacy: .public)s); transcribing")
            // Key released — bring the volume back right away; transcription
            // and insertion don't need quiet.
            SystemAudioDucker.shared.restore()
            state = .processing
            level = 0
            let screenContext = pendingScreenContext
            pendingScreenContext = nil
            await runPipeline(utterance, screenContext: screenContext)
        }
    }

    private func runPipeline(_ utterance: Utterance, screenContext: ScreenContextSnapshot? = nil) async {
        // A warm engine finishes in ~110 ms; keep the "processing…" lozenge up
        // for a beat so it reads as a state, not a flicker. (Cold runs take
        // seconds and are unaffected.)
        let processingShownAt = ContinuousClock.now
        do {
            let clock = ContinuousClock()
            var stageStart = clock.now
            // Parakeet output is verbatim WITH native punctuation; it serves
            // as both the polish input and the fail-open fallback.
            let raw = try await transcriber.transcribe(utterance)
            var polishInput = raw
            if AppSettings.screenTerminologyEnabled {
                let dictionary = AppSettings.loadDictionary()
                // Apply the personal dictionary first, then protect its chosen
                // spellings from contextual correction.
                polishInput = ReplacementEngine(dictionary: dictionary).apply(to: polishInput)
                let correction = TerminologyCorrector.correct(
                    polishInput,
                    screenTerms: screenContext?.terms ?? [],
                    learnedTerms: LearnedTerminologyStore.load(),
                    protectedTerms: dictionary.rules.map(\.written))
                polishInput = correction.text
                LearnedTerminologyStore.learn(
                    correction.matches, sourceBundleID: screenContext?.bundleID)
                if !correction.matches.isEmpty {
                    Self.logger.info("terminology corrections applied: \(correction.matches.count, privacy: .public)")
                }
            }
            let formatted = polishInput
            // Raw transcript at debug level: when a user reports "that's not
            // what I said," this attributes the error to ASR vs polish in
            // seconds instead of a reconstruction hunt.
            Self.logger.debug("raw transcript: \"\(raw, privacy: .public)\"")
            let transcribeDuration = clock.now - stageStart

            stageStart = clock.now
            // Always runs: dictionary replacements apply even with LLM polish
            // off — the polisher's own llmEnabled config gates the model pass.
            // S1 works from the RAW transcript (punctuates better than the
            // pause-based formatter); the formatter output is the fallback.
            let context = PolishContext(
                targetAppBundleID: screenContext?.bundleID
                    ?? NSWorkspace.shared.frontmostApplication?.bundleIdentifier
            )
            let text: String
            if let localPolisher = polisher as? LocalPolisher {
                text = await localPolisher.polishTranscript(
                    raw: polishInput, formatted: formatted, context: context)
            } else {
                text = await polisher.polish(formatted, context: context)
            }
            let polishDuration = clock.now - stageStart

            stageStart = clock.now
            try await inserter.insert(text)
            let insertDuration = clock.now - stageStart
            Self.logger.info("""
                stages: transcribe \(String(describing: transcribeDuration), privacy: .public), \
                polish \(String(describing: polishDuration), privacy: .public), \
                insert \(String(describing: insertDuration), privacy: .public)
                """)
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
        trimHistory()
    }

    private func trimHistory() {
        let limit = AppSettings.historyLimit
        if history.count > limit {
            history.removeLast(history.count - limit)
        }
    }

    func clearHistory() {
        history.removeAll()
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
    private let primary: FrontmostInserter
    private let fallback = PasteboardInserter()

    init(configuration: InserterConfiguration = .default) {
        primary = FrontmostInserter(configuration: configuration)
    }

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
