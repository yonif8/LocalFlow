import Foundation
import LFContracts
import LFPolish

// polish-cli — demonstrates both LFPolish stages.
//
// usage: polish-cli [--no-llm] [--timeout <seconds>] [--punctuation]
//                   [--target <bundle-id>] [text ...]
// Reads text from arguments, or from stdin when no text arguments are given.

var llmEnabled = true
var timeout: TimeInterval = 2.0
var punctuation = false
var targetBundleID: String? = nil
var textParts: [String] = []

var args = Array(CommandLine.arguments.dropFirst())
while !args.isEmpty {
    let arg = args.removeFirst()
    switch arg {
    case "--no-llm":
        llmEnabled = false
    case "--timeout":
        guard !args.isEmpty, let value = TimeInterval(args.removeFirst()) else {
            FileHandle.standardError.write(Data("error: --timeout needs a number\n".utf8))
            exit(2)
        }
        timeout = value
    case "--punctuation":
        punctuation = true
    case "--target":
        guard !args.isEmpty else {
            FileHandle.standardError.write(Data("error: --target needs a bundle id\n".utf8))
            exit(2)
        }
        targetBundleID = args.removeFirst()
    case "--help", "-h":
        print("""
        usage: polish-cli [--no-llm] [--timeout <seconds>] [--punctuation] \
        [--target <bundle-id>] [text ...]
        Reads text from arguments, or stdin when none are given.
        """)
        exit(0)
    default:
        textParts.append(arg)
    }
}

var input = textParts.joined(separator: " ")
if input.isEmpty {
    input = (try? String(data: FileHandle.standardInput.readToEnd() ?? Data(), encoding: .utf8))
        .flatMap { $0 } ?? ""
    input = input.trimmingCharacters(in: .whitespacesAndNewlines)
}
if input.isEmpty {
    input = "um so check out echidna cams comma i mean local flow uh it runs fully on device"
    print("(no input given; using demo text)\n")
}

// Demo personal dictionary: written to JSON and loaded back, to exercise the
// same persistence path the app will use.
let demoDictionary = PersonalDictionary(
    rules: [
        ReplacementRule(spoken: "echidna cams", written: "EchidnaCams"),
        ReplacementRule(spoken: "local flow", written: "LocalFlow"),
        ReplacementRule(spoken: "i b m", written: "IBM"),
    ],
    spokenPunctuationEnabled: punctuation)

let dictionaryURL = FileManager.default.temporaryDirectory
    .appendingPathComponent("polish-cli-dictionary.json")
try demoDictionary.save(to: dictionaryURL)
let loadedDictionary = try PersonalDictionary.load(from: dictionaryURL)

let polisher = LocalPolisher(
    dictionary: loadedDictionary,
    configuration: .init(llmEnabled: llmEnabled, timeout: timeout))

print("dictionary        : \(loadedDictionary.rules.count) rules (JSON at \(dictionaryURL.path))")
print("spoken punctuation: \(loadedDictionary.spokenPunctuationEnabled ? "on" : "off")")
print("model availability: \(polisher.modelAvailabilityDescription)")
print("llm pass          : \(llmEnabled ? "enabled (timeout \(timeout)s)" : "disabled (--no-llm)")")
print("target app        : \(targetBundleID ?? "none (neutral tone)")")
print()

let context = PolishContext(targetAppBundleID: targetBundleID)

// Warm the model the way the app does at startup (load + first generation),
// so the timed run below reflects steady-state latency, not cold start.
if llmEnabled {
    let warmStart = ContinuousClock.now
    _ = await LocalPolisher(
        configuration: .init(llmEnabled: true, timeout: 30)
    ).polish("warm up run", context: context)
    let warmSeconds = Double((ContinuousClock.now - warmStart).components.seconds)
    print("model warm-up     : \(String(format: "%.1fs", warmSeconds)) (one-time, app does this at launch)")
}

let result = await polisher.polishDetailed(input, context: context)

func format(_ duration: Duration) -> String {
    let seconds = Double(duration.components.seconds)
        + Double(duration.components.attoseconds) / 1e18
    return String(format: "%.1f ms", seconds * 1000)
}

print("input             : \(input)")
print("after replacements: \(result.afterReplacements)  [\(format(result.replacementsDuration))]")
switch result.outcome {
case .polished:
    let timing = result.llmDuration.map(format) ?? "?"
    print("after FM polish   : \(result.text)  [\(timing)]")
case .replacementsOnly(let reason):
    let timing = result.llmDuration.map { "  [\(format($0))]" } ?? ""
    print("after FM polish   : (skipped, fail-open: \(reason))\(timing)")
    print("final text        : \(result.text)")
}
