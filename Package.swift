// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "LocalFlow",
    platforms: [.macOS("15.0")],
    dependencies: [
        .package(url: "https://github.com/kylehowells/Granite-MLX", from: "0.1.1"),
    ],
    targets: [
        // Shared contracts — FROZEN. Only the orchestrator edits this target.
        .target(name: "LFContracts"),

        // Stream A: ASR engine (Granite-MLX, MLX backend + punctuation formatter)
        .target(
            name: "LFEngine",
            dependencies: [
                "LFContracts",
                .product(name: "GraniteMLX", package: "Granite-MLX"),
            ]
        ),
        .executableTarget(name: "engine-cli", dependencies: ["LFEngine"]),

        // Stream B: hold-to-talk hotkey + mic capture
        .target(name: "LFCapture", dependencies: ["LFContracts"]),
        .executableTarget(name: "capture-cli", dependencies: ["LFCapture"]),

        // Stream C: text insertion into frontmost app
        .target(name: "LFInsert", dependencies: ["LFContracts"]),
        .executableTarget(name: "insert-cli", dependencies: ["LFInsert"]),

        // Stream D: dictionary/replacements + Apple Foundation Models polish
        .target(name: "LFPolish", dependencies: ["LFContracts"]),
        .executableTarget(name: "polish-cli", dependencies: ["LFPolish"]),

        // Stream E: menu bar app shell (engine wired in by orchestrator at integration)
        .executableTarget(
            name: "LocalFlowApp",
            dependencies: ["LFContracts", "LFCapture", "LFInsert", "LFPolish"]
        ),

        .testTarget(name: "LFEngineTests", dependencies: ["LFEngine"]),
        .testTarget(name: "LFPolishTests", dependencies: ["LFPolish"]),
        .testTarget(name: "LFInsertTests", dependencies: ["LFInsert"]),
    ]
)
