import AVFoundation
import Foundation
import LFCapture
import LFContracts

// capture-cli — interactive verification tool for the LFCapture hold-to-talk pipeline.
//
//   swift run capture-cli               hold Fn (Globe) to record, release to save a wav
//   swift run capture-cli --fake        skip the event tap, record 3 s straight from the mic
//
// Options:
//   --hotkey fn|rcmd|ropt   hold-to-talk key (default fn)
//   --no-prewarm            don't keep the audio engine warm between utterances
//   --seconds N             fake-mode recording length (default 3)

let args = CommandLine.arguments.dropFirst()

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data(("capture-cli: " + message + "\n").utf8))
    exit(1)
}

var fakeMode = false
var hotkey: HotkeyKey = .fn
var prewarm = true
var fakeSeconds = 3.0

var it = args.makeIterator()
while let arg = it.next() {
    switch arg {
    case "--fake":
        fakeMode = true
    case "--no-prewarm":
        prewarm = false
    case "--hotkey":
        switch it.next() {
        case "fn": hotkey = .fn
        case "rcmd": hotkey = .rightCommand
        case "ropt": hotkey = .rightOption
        case let other: fail("--hotkey expects fn|rcmd|ropt, got \(other ?? "nothing")")
        }
    case "--seconds":
        guard let raw = it.next(), let secs = Double(raw), secs > 0 else {
            fail("--seconds expects a positive number")
        }
        fakeSeconds = secs
    case "--help", "-h":
        print("usage: capture-cli [--fake] [--hotkey fn|rcmd|ropt] [--no-prewarm] [--seconds N]")
        exit(0)
    default:
        fail("unknown argument \(arg) (try --help)")
    }
}

// MARK: - Helpers

let timeFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f
}()

func ts() -> String { "[\(timeFormatter.string(from: Date()))]" }

func log(_ message: String) { print("\(ts()) \(message)") }

func nextWavURL() -> URL {
    let cwd = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    var n = 1
    while true {
        let url = cwd.appendingPathComponent(String(format: "capture-%03d.wav", n))
        if !FileManager.default.fileExists(atPath: url.path) { return url }
        n += 1
    }
}

func meterBar(_ level: Float) -> String {
    let width = 24
    let filled = Int((level * Float(width)).rounded())
    return "[" + String(repeating: "#", count: filled)
        + String(repeating: "-", count: width - filled)
        + String(format: "] %.2f", level)
}

/// Check/report both permissions. Returns (micOK, inputMonitoringOK).
func reportPermissions(needTap: Bool) async -> (Bool, Bool) {
    var micOK: Bool
    switch CapturePermissions.microphoneStatus() {
    case .authorized:
        print("Microphone permission: granted")
        micOK = true
    case .notDetermined:
        print("Microphone permission: not determined — requesting (watch for the system prompt)...")
        micOK = await CapturePermissions.requestMicrophone()
        print("Microphone permission: \(micOK ? "granted" : "DENIED")")
    default:
        print("Microphone permission: DENIED")
        print("  -> System Settings > Privacy & Security > Microphone: enable your terminal app, then re-run.")
        micOK = false
    }

    var tapOK = true
    if needTap {
        tapOK = CapturePermissions.inputMonitoringGranted()
        if tapOK {
            print("Input Monitoring permission: granted")
        } else {
            print("Input Monitoring permission: NOT granted (needed for the hotkey event tap)")
            print("  -> System Settings > Privacy & Security > Input Monitoring: enable your terminal app")
            print("     (e.g. Terminal or iTerm2), then quit and re-run capture-cli.")
            print("  Requesting now so the app appears in that list...")
            CapturePermissions.requestInputMonitoring()
        }
    }
    return (micOK, tapOK)
}

// MARK: - Fake mode (no event tap; verifies mic pipeline + wav output)

if fakeMode {
    print("capture-cli --fake: recording \(String(format: "%.1f", fakeSeconds)) s from the default input, no event tap.")
    let (micOK, _) = await reportPermissions(needTap: false)
    guard micOK else { exit(1) }

    let recorder = MicRecorder()
    do {
        try recorder.prepare(keepWarm: false)
        log("recording  (speak now)")
        try recorder.beginRecording()
    } catch {
        fail(String(describing: error))
    }
    try? await Task.sleep(for: .seconds(fakeSeconds))
    let samples = recorder.endRecording()
    recorder.shutdown()

    let duration = Double(samples.count) / MicRecorder.targetSampleRate
    let peak = samples.map(abs).max() ?? 0
    let rms = samples.isEmpty ? 0 : (samples.reduce(Float(0)) { $0 + $1 * $1 } / Float(samples.count)).squareRoot()
    let url = nextWavURL()
    do {
        try WavWriter.write(samples: samples, sampleRate: Int(MicRecorder.targetSampleRate), to: url)
    } catch {
        fail("could not write wav: \(error)")
    }
    log(String(format: "ended      %.2fs, %d samples, peak %.3f, rms %.4f -> %@",
               duration, samples.count, peak, rms, url.lastPathComponent))
    if peak < 0.001 {
        print("warning: audio is silent — check the input device and mic permission.")
    }
    exit(0)
}

// MARK: - Interactive hold-to-talk mode

print("capture-cli: hold-to-talk capture test")
print("  hotkey: hold \(hotkey)   cancel: Esc while recording   quit: Ctrl-C")
print("  prewarm: \(prewarm ? "on (engine stays running between utterances)" : "off")")
print("")

let (micOK, tapOK) = await reportPermissions(needTap: true)
guard micOK, tapOK else { exit(1) }
print("")

let config = HotkeyConfig(key: hotkey, keepMicWarm: prewarm)
let engine = HoldToTalkCaptureEngine(config: config)
do {
    try engine.start()
} catch {
    fail(String(describing: error))
}

log("armed      hold \(hotkey) and speak; release to save")

var recordingStart: Date?
var lastMeterPrint = Date.distantPast

for await event in engine.events {
    switch event {
    case .began:
        recordingStart = Date()
        log("recording  (hotkey down)")

    case .level(let level):
        // Throttle the meter to ~10 Hz and redraw in place.
        let now = Date()
        if now.timeIntervalSince(lastMeterPrint) >= 0.1 {
            lastMeterPrint = now
            print("\r\(ts()) level      \(meterBar(level))", terminator: "")
            fflush(stdout)
        }

    case .ended(let utterance):
        print("") // finish the meter line
        let held = recordingStart.map { Date().timeIntervalSince($0) } ?? 0
        recordingStart = nil
        let url = nextWavURL()
        do {
            try WavWriter.write(
                samples: utterance.samples,
                sampleRate: Int(utterance.sampleRate),
                to: url
            )
            log(String(format: "ended      audio %.2fs (held %.2fs, %d samples @ %.0f Hz) -> %@",
                       utterance.duration, held, utterance.samples.count,
                       utterance.sampleRate, url.lastPathComponent))
        } catch {
            log("ended      but failed to write wav: \(error)")
        }

    case .cancelled:
        print("")
        recordingStart = nil
        log("cancelled  (Esc, tap too short, or utterance under minimum)")
    }
}

log("event stream ended")
