import Foundation
import LFInsert

// insert-cli — manual test harness for LFInsert.
//
//   insert-cli "some text" [--delay 3] [--strategy ax|paste|type|auto]
//   insert-cli --doctor [--delay 3]
//
// Counts down so you can focus the target app, then inserts (or, with
// --doctor, prints diagnostics about the focused element).

struct CLIOptions {
    var text: String?
    var delay: TimeInterval = 3
    var strategy: String = "auto"
    var doctor = false
}

func parseArguments(_ args: [String]) -> CLIOptions? {
    var options = CLIOptions()
    var index = 0
    while index < args.count {
        let arg = args[index]
        switch arg {
        case "--doctor":
            options.doctor = true
        case "--delay":
            index += 1
            guard index < args.count, let value = TimeInterval(args[index]), value >= 0 else {
                FileHandle.standardError.write(Data("error: --delay needs a non-negative number\n".utf8))
                return nil
            }
            options.delay = value
        case "--strategy":
            index += 1
            guard index < args.count, ["ax", "paste", "type", "auto"].contains(args[index]) else {
                FileHandle.standardError.write(Data("error: --strategy must be ax|paste|type|auto\n".utf8))
                return nil
            }
            options.strategy = args[index]
        case "--help", "-h":
            return nil
        default:
            if options.text == nil && !arg.hasPrefix("--") {
                options.text = arg
            } else {
                FileHandle.standardError.write(Data("error: unexpected argument '\(arg)'\n".utf8))
                return nil
            }
        }
        index += 1
    }
    return options
}

func printUsage() {
    print("""
    usage: insert-cli "text" [--delay 3] [--strategy ax|paste|type|auto]
           insert-cli --doctor [--delay 3]

      --delay N     seconds to wait before acting, so you can focus the target app (default 3)
      --strategy S  ax    = AX selected-text only
                    paste = pasteboard + Cmd-V only
                    type  = synthetic unicode typing only
                    auto  = ax then paste (default)
      --doctor      print frontmost app, focused AX element info, secure-input status
    """)
}

func strategyOrder(for name: String) -> [InsertionStrategy] {
    switch name {
    case "ax": return [.ax]
    case "paste": return [.paste]
    case "type": return [.type]
    default: return InserterConfiguration.default.strategyOrder
    }
}

func countdown(_ seconds: TimeInterval, message: String) async {
    guard seconds > 0 else { return }
    print(message)
    var remaining = Int(seconds.rounded())
    while remaining > 0 {
        print("  \(remaining)...", terminator: " ")
        fflush(stdout)
        try? await Task.sleep(nanoseconds: 1_000_000_000)
        remaining -= 1
    }
    print("")
}

// MARK: - Entry point (top-level async)

let arguments = Array(CommandLine.arguments.dropFirst())
guard let options = parseArguments(arguments) else {
    printUsage()
    exit(64) // EX_USAGE
}

// Permission report (prompting adds this terminal to the Accessibility pane).
let axGranted = InsertPermissions.accessibilityGranted()
print("Accessibility permission: \(axGranted ? "GRANTED" : "NOT GRANTED")")
let secureInput = InsertPermissions.secureInputEnabled
print("Secure Keyboard Entry:    \(secureInput ? "ENABLED (insertion may be blocked; check Terminal's Secure Keyboard Entry menu item or an open password prompt)" : "off")")

if !axGranted {
    _ = InsertPermissions.accessibilityGranted(promptIfNeeded: true)
    print("""

    This tool needs Accessibility permission (for both AX insertion and
    synthetic keystrokes). Grant it to the app running this command
    (e.g. Terminal / iTerm2 / your IDE) in:
      System Settings > Privacy & Security > Accessibility
    then re-run. A system prompt may have just appeared to add it for you.
    """)
    exit(1)
}

if options.doctor {
    await countdown(options.delay, message: "Focus the app you want to inspect...")
    let report = InsertDoctorReport.gather() // top-level code is MainActor-isolated
    print("--- insert doctor ---")
    print("frontmost app:            \(report.frontmostAppName ?? "<unknown>") (\(report.frontmostBundleID ?? "<no bundle id>"))")
    if let err = report.focusedElementLookupError {
        print("focused AX element:       <unavailable> (AXError \(err))")
    } else {
        print("focused AX element role:  \(report.focusedElementRole ?? "<none>")")
        print("focused AX subrole:       \(report.focusedElementSubrole ?? "<none>")")
        print("AXSelectedText settable:  \(report.selectedTextSettable)")
    }
    print("secure keyboard entry:    \(report.secureInputEnabled)")
    print("accessibility granted:    \(report.accessibilityGranted)")
    exit(0)
}

guard let text = options.text, !text.isEmpty else {
    printUsage()
    exit(64)
}

let configuration = InserterConfiguration(strategyOrder: strategyOrder(for: options.strategy))
let inserter = FrontmostInserter(configuration: configuration)

await countdown(options.delay, message: "Focus the target text field... inserting in:")

do {
    let outcome = try await inserter.insertReporting(text)
    print("Inserted via strategy: \(outcome.strategy)")
    if !outcome.earlierFailures.isEmpty {
        for failure in outcome.earlierFailures {
            print("  (fell through \(failure.strategy.rawValue): \(failure.reason))")
        }
    }
    if outcome.secureInputWasEnabled {
        print("  note: Secure Keyboard Entry was enabled during insertion.")
    }
    exit(0)
} catch {
    let message = (error as? InsertionError).map(String.init(describing:)) ?? "\(error)"
    FileHandle.standardError.write(Data("Insertion failed: \(message)\n".utf8))
    exit(1)
}
