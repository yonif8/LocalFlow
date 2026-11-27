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
        case done(String)
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
    /// This press had no editable field focused and "Send to WhatsApp" is on:
    /// the result goes to WhatsApp with the key-down screenshot.
    private(set) var routingToWhatsApp = false
    private var whatsAppScreenshot: Task<Data, Error>?
    /// Key-down focus check; only `.notEditable` routes to WhatsApp.
    private var whatsAppFocusProbe: Task<FocusState, Never>?

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
    private var screenContextSessionID: UUID?
    private var screenContextTask: Task<Void, Never>?
    private var errorResetTask: Task<Void, Never>?
    private var didPrepareTranscriber = false
    private var incremental: IncrementalDictation?
    private var processedSamples = 0
    private var segmentStartedAt = ContinuousClock.now
    private var quietSince: ContinuousClock.Instant?
    private var dictationTargetBundleID: String?
    private var segmentBudget = DictationSegmentBudget()

    private func resetSegments() {
        let old = incremental
        incremental = nil
        if let old { Task { await old.cancel() } }
        processedSamples = 0
        quietSince = nil
        segmentStartedAt = .now
    }

    private func considerSegment(level: Float) async {
        guard state == .recording, let capture = realCapture else { return }
        let now = ContinuousClock.now
        if level > 0.18 { quietSince = nil; return }
        if quietSince == nil { quietSince = now }
        guard let quietSince, now - quietSince >= .milliseconds(650),
              now - segmentStartedAt >= .seconds(segmentBudget.pauseSearchSeconds) else { return }
        let chunk = capture.snapshot(from: processedSamples)
        guard chunk.duration >= segmentBudget.pauseSearchSeconds,
              DictationSegmentBudget.hasQuietTail(chunk.samples, sampleRate: chunk.sampleRate) else { return }
        if incremental == nil {
            let target = dictationTargetBundleID
            let screenContext = pendingScreenContext
            incremental = IncrementalDictation(transcriber: transcriber, timing: { [weak self] audio, elapsed in
                await self?.observeSegment(audio: audio, elapsed: elapsed)
            }) { [weak self] raw in
                guard let self else { throw CancellationError() }
                try Task.checkCancellation()
                return await self.polishRecognized(raw, screenContext: screenContext,
                                                  targetBundleID: target)
            }
        }
        processedSamples += chunk.samples.count
        segmentStartedAt = now
        self.quietSince = nil
        Self.logger.info("Background chunk: audioSeconds=\(chunk.duration, privacy: .public) sampleOffset=\(self.processedSamples, privacy: .public) pauseSearchSeconds=\(self.segmentBudget.pauseSearchSeconds, privacy: .public)")
        await incremental?.append(chunk)
    }

    private func observeSegment(audio: Double, elapsed: Double) {
        segmentBudget.observe(audioSeconds: audio, processingSeconds: elapsed)
        Self.logger.info("Background chunk finished: audioSeconds=\(audio, privacy: .public) processingSeconds=\(elapsed, privacy: .public) nextPauseSearchSeconds=\(self.segmentBudget.pauseSearchSeconds, privacy: .public)")
    }

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
        observeAppActivationForFocusProbe()
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
        case .done: return isListening ? "waveform" : "waveform.slash"
        case .error: return "mic.badge.xmark"
        }
    }

    func startListening() {
        guard !isListening else { return }

        // Existing installations may already have screen terminology enabled
        // from before OCR was introduced. Ask once through macOS rather than
        // silently leaving those users on metadata-only context.
        if AppSettings.screenTerminologyEnabled, !ScreenOCR.hasPermission {
            _ = ScreenOCR.requestPermission()
        }

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

        // Send to WhatsApp needs a readable focus; build the current app's
        // accessibility tree now rather than at the first key-down.
        if AppSettings.whatsAppSendEnabled,
           let pid = NSWorkspace.shared.frontmostApplication?.processIdentifier {
            EditableFocusPolicy.enableAccessibilityTree(processID: pid)
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
        resetSegments()
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
            resetSegments()
            dictationTargetBundleID = NSWorkspace.shared.frontmostApplication?.bundleIdentifier
            Self.logger.info("capture began")
            errorResetTask?.cancel()
            beginWhatsAppRoutingIfNeeded()
            if AppSettings.screenTerminologyEnabled {
                let sessionID = UUID()
                screenContextSessionID = sessionID
                screenContextTask?.cancel()
                let targetApp = NSWorkspace.shared.frontmostApplication
                if let processID = targetApp?.processIdentifier,
                   targetApp?.bundleIdentifier != Bundle.main.bundleIdentifier,
                   ScreenOCR.hasPermission {
                    screenContextTask = Task { [weak self] in
                        do {
                            let result = try await ScreenOCR.capture(processID: processID)
                            guard let self, self.screenContextSessionID == sessionID else { return }
                            if let existing = self.pendingScreenContext {
                                self.pendingScreenContext = existing.mergingOCR(result)
                            }
                            Self.logger.info("""
                                OCR context: \(result.terms.count, privacy: .public) terms from \
                                \(result.recognizedLines, privacy: .public) lines in \
                                \(String(describing: result.elapsed), privacy: .public)
                                """)
                        } catch {
                            guard !Task.isCancelled else { return }
                            Self.logger.info("OCR context unavailable: \(String(describing: error), privacy: .public)")
                        }
                    }
                } else {
                    screenContextTask = nil
                    if !ScreenOCR.hasPermission {
                        Self.logger.info("OCR context unavailable: screen recording permission not granted")
                    }
                }
                let snapshot = ScreenContextCollector.capture()
                pendingScreenContext = snapshot
                Self.logger.info("""
                    screen context: \(snapshot.terms.count, privacy: .public) terms from \
                    \(snapshot.visitedElements, privacy: .public) elements in \
                    \(String(describing: snapshot.elapsed), privacy: .public)
                    """)
            } else {
                pendingScreenContext = nil
                screenContextSessionID = nil
                screenContextTask?.cancel()
                screenContextTask = nil
            }
            state = .recording
            level = 0
            if AppSettings.duckWhileDictating {
                SystemAudioDucker.shared.duck(
                    to: Float32(AppSettings.duckLevelPercent) / 100)
            }
            HUDController.shared.show()

        case .level(let value):
            await considerSegment(level: value)
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
            resetSegments()
            endWhatsAppRouting()
            SystemAudioDucker.shared.restore()
            state = .idle
            level = 0
            HUDController.shared.hide()
            pendingScreenContext = nil
            screenContextSessionID = nil
            screenContextTask?.cancel()
            screenContextTask = nil

        case .ended(let utterance):
            Self.logger.info("capture ended (\(utterance.duration, privacy: .public)s); transcribing")
            // Key released — bring the volume back right away; transcription
            // and insertion don't need quiet.
            SystemAudioDucker.shared.restore()
            state = .processing
            level = 0
            await runPipeline(utterance)
        }
    }

    private func runPipeline(_ utterance: Utterance) async {
        defer {
            resetSegments()
            endWhatsAppRouting()
            pendingScreenContext = nil
            screenContextSessionID = nil
            screenContextTask?.cancel()
            screenContextTask = nil
        }
        // A warm engine finishes in ~110 ms; keep the "processing…" lozenge up
        // for a beat so it reads as a state, not a flicker. (Cold runs take
        // seconds and are unaffected.)
        let processingShownAt = ContinuousClock.now
        let runID = UUID().uuidString
        Self.logger.info("Pipeline \(runID, privacy: .public): ASR begin samples=\(utterance.samples.count, privacy: .public) screenTermsEnabled=\(AppSettings.screenTerminologyEnabled, privacy: .public)")
        do {
            let clock = ContinuousClock()
            let stageStart = clock.now
            let screenContext = pendingScreenContext
            let text: String
            if let incremental {
                let offset = min(processedSamples, utterance.samples.count)
                let remainder = Utterance(samples: Array(utterance.samples[offset...]),
                                          sampleRate: utterance.sampleRate)
                do {
                    text = try await incremental.finish(remainder)
                } catch {
                    try Task.checkCancellation()
                    Self.logger.warning("Background segment failed; retrying retained complete audio")
                    let raw = try await transcriber.transcribe(utterance)
                    text = await polishRecognized(raw, screenContext: screenContext,
                                                 targetBundleID: dictationTargetBundleID)
                }
            } else {
                let raw = try await transcriber.transcribe(utterance)
                text = await polishRecognized(raw, screenContext: screenContext,
                                             targetBundleID: dictationTargetBundleID)
            }
            try Task.checkCancellation()
            let processingDuration = clock.now - stageStart
            if incremental == nil {
                let elapsed = Double(processingDuration.components.seconds)
                    + Double(processingDuration.components.attoseconds) / 1e18
                if utterance.duration >= 12 { observeSegment(audio: utterance.duration, elapsed: elapsed) }
            }
            let insertionStart = clock.now
            Self.logger.info("Pipeline \(runID, privacy: .public): polish complete; insertion begin characters=\(text.count, privacy: .public)")
            if await resolveWhatsAppRouting(), let screenshot = whatsAppScreenshot {
                guard await sendToWhatsApp(text, screenshot: screenshot, runID: runID) else { return }
            } else {
                try await inserter.insert(text)
            }
            let insertDuration = clock.now - insertionStart
            Self.logger.info("""
                stages: release processing \(String(describing: processingDuration), privacy: .public), \
                insert \(String(describing: insertDuration), privacy: .public)
                """)
            appendHistory(text)
            Self.logger.info("Pipeline \(runID, privacy: .public): complete")
            let elapsed = ContinuousClock.now - processingShownAt
            if elapsed < .milliseconds(350) {
                try? await Task.sleep(for: .milliseconds(350) - elapsed)
            }
            if routingToWhatsApp {
                flash(.done("Sent to WhatsApp"))
            } else {
                state = .idle
                HUDController.shared.hide()
            }
        } catch {
            Self.logger.error("Pipeline \(runID, privacy: .public): failed domain=\((error as NSError).domain, privacy: .public) code=\((error as NSError).code, privacy: .public)")
            surfaceError("Dictation failed: \(error.localizedDescription)")
        }
    }

    private func polishRecognized(_ raw: String, screenContext: ScreenContextSnapshot?,
                                  targetBundleID: String?) async -> String {
        guard !Task.isCancelled else { return raw }
        let runID = UUID().uuidString
            var polishInput = raw
            var terminologyMatches: [TerminologyMatch] = []
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
                terminologyMatches = correction.matches
                if state != .recording && !Task.isCancelled && !ProcessInfo.processInfo.arguments.contains("--diagnostic-mode") {
                    LearnedTerminologyStore.learn(
                        correction.matches, sourceBundleID: screenContext?.bundleID)
                }
                if !correction.matches.isEmpty {
                    let screenCount = correction.matches.filter { $0.source == .screen }.count
                    let learnedCount = correction.matches.count - screenCount
                    Self.logger.info("""
                        terminology correction completed \
                        (screen=\(screenCount, privacy: .public), learned=\(learnedCount, privacy: .public))
                        """)
                }
            }
            let formatted = polishInput
            // Metadata only; never log dictated words.
            Self.logger.info("Pipeline \(runID, privacy: .public): terminology complete; polish begin")

            // Always runs: dictionary replacements apply even with LLM polish
            // off — the polisher's own llmEnabled config gates the model pass.
            // Parakeet supplies punctuation; S1 cleans up the recognized text.
            let context = PolishContext(
                targetAppBundleID: screenContext?.bundleID
                    ?? targetBundleID
            )
            let polishedText: String
            if let localPolisher = polisher as? LocalPolisher {
                polishedText = await localPolisher.polishTranscript(
                    raw: polishInput, formatted: formatted, context: context)
            } else {
                polishedText = await polisher.polish(formatted, context: context)
            }
            // Reassert only terms that actually matched before polishing—not
            // the entire screen or learned dictionary. This prevents stale
            // context from introducing a new change after S1 cleaned the text.
            let text: String
            if !terminologyMatches.isEmpty {
                let matchedTerms = terminologyMatches.map {
                    LearnedTerm(canonical: $0.canonical, aliases: [$0.heard])
                }
                let finalCorrection = TerminologyCorrector.correct(
                    polishedText,
                    screenTerms: [],
                    learnedTerms: matchedTerms,
                    protectedTerms: AppSettings.loadDictionary().rules.map(\.written))
                text = finalCorrection.text
                if text != polishedText {
                    Self.logger.info("post-polish terminology restored; characters=\(text.count, privacy: .public)")
                }
            } else {
                text = polishedText
            }

        return text
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
        flash(.error(message))
    }

    /// Shows a terminal HUD state briefly, then returns to idle.
    private func flash(_ terminal: State) {
        state = terminal
        HUDController.shared.show()
        errorResetTask?.cancel()
        errorResetTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2.5))
            guard let self, !Task.isCancelled else { return }
            if self.state == terminal {
                self.state = .idle
                HUDController.shared.hide()
            }
        }
    }

    // MARK: Send to WhatsApp

    /// Key-down: when the feature is set up, start the screenshot (it must
    /// show the screen as it was at key-down) and a focus probe in parallel.
    /// The dictation goes to WhatsApp only if the probe POSITIVELY finds no
    /// text box; an unreadable focus falls back to normal dictation.
    private func beginWhatsAppRoutingIfNeeded() {
        endWhatsAppRouting()
        guard AppSettings.whatsAppSendReady,
              NSWorkspace.shared.frontmostApplication?.bundleIdentifier != Bundle.main.bundleIdentifier
        else { return }
        whatsAppScreenshot = Task { try await ScreenSnapshot.captureDisplayPNG() }
        whatsAppFocusProbe = Task { @MainActor [weak self] in
            let state = await EditableFocusPolicy.probe()
            guard !Task.isCancelled else { return state }
            self?.routingToWhatsApp = state == .notEditable
            Self.logger.info("WhatsApp focus probe: \(String(describing: state), privacy: .public)")
            return state
        }
    }

    /// Release: wait for the (bounded) focus probe and decide.
    private func resolveWhatsAppRouting() async -> Bool {
        guard let probe = whatsAppFocusProbe else { return false }
        let state = await probe.value
        routingToWhatsApp = state == .notEditable
        if !routingToWhatsApp { whatsAppScreenshot?.cancel() }
        return routingToWhatsApp
    }

    /// While the feature is on, ask each app brought to the front to build
    /// its accessibility tree so the key-down probe can answer at once.
    private func observeAppActivationForFocusProbe() {
        NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didActivateApplicationNotification, object: nil, queue: .main
        ) { note in
            guard let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication
            else { return }
            let pid = app.processIdentifier
            MainActor.assumeIsolated {
                guard AppSettings.whatsAppSendEnabled else { return }
                EditableFocusPolicy.enableAccessibilityTree(processID: pid)
            }
        }
    }

    private func endWhatsAppRouting() {
        whatsAppScreenshot?.cancel()
        whatsAppScreenshot = nil
        whatsAppFocusProbe?.cancel()
        whatsAppFocusProbe = nil
        routingToWhatsApp = false
    }

    /// Sends screenshot + text. On any failure the transcript is copied to the
    /// clipboard and kept in history so nothing the user said is lost.
    /// Returns false when it already surfaced an error.
    private func sendToWhatsApp(_ text: String, screenshot: Task<Data, Error>,
                                runID: String) async -> Bool {
        do {
            let png = try await screenshot.value
            try await WhatsAppSender().send(png: png, caption: text,
                                            to: AppSettings.whatsAppRecipient)
            Self.logger.info("Pipeline \(runID, privacy: .public): sent to WhatsApp characters=\(text.count, privacy: .public)")
            return true
        } catch {
            Self.logger.error("Pipeline \(runID, privacy: .public): WhatsApp send failed: \(String(describing: error), privacy: .public)")
            copyToClipboard(Transcript(text: text, date: Date()))
            appendHistory(text)
            surfaceError("Not sent to WhatsApp. Text copied.")
            return false
        }
    }

    /// Settings → "Send test message": same path as a real dictation.
    func sendWhatsAppTest() async -> String {
        do {
            let png = try await ScreenSnapshot.captureDisplayPNG()
            try await WhatsAppSender().send(png: png, caption: "LocalFlow test message",
                                            to: AppSettings.whatsAppRecipient)
            return "Sent. Check WhatsApp."
        } catch {
            return "Not sent: \(String(describing: error))"
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
