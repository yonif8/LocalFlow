// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "LocalFlow",
    platforms: [.macOS("15.0")],
    dependencies: [
        // Polish LLM runtime (S1-mini). 3.x versions track mlx-swift 0.31.x.
        .package(url: "https://github.com/ml-explore/mlx-swift-lm.git", exact: "3.31.4"),
        .package(url: "https://github.com/huggingface/swift-huggingface", from: "0.9.0"),
        .package(url: "https://github.com/huggingface/swift-transformers", from: "1.3.0"),
        // The ASR engine (Parakeet TDT v3, CoreML): verbatim transcripts
        // with native punctuation.
        .package(url: "https://github.com/FluidInference/FluidAudio.git", from: "0.12.4"),
        // Auto-updates. 2.9.5+ contains security fixes; EdDSA-signed updates
        // work without Apple-trusted code signing.
        .package(url: "https://github.com/sparkle-project/Sparkle", from: "2.9.6"),
    ],
    targets: [
        // Shared contracts — FROZEN. Only the orchestrator edits this target.
        .target(name: "LFContracts"),

        // Stream A: ASR engine (Granite-MLX, MLX backend + punctuation formatter)
        .target(
            name: "LFEngine",
            dependencies: [
                "LFContracts",
                .product(name: "FluidAudio", package: "FluidAudio"),
            ]
        ),
        .executableTarget(name: "engine-cli", dependencies: ["LFEngine"]),

        // Stream B: hold-to-talk hotkey + mic capture
        .target(name: "LFCapture", dependencies: ["LFContracts"]),
        .executableTarget(name: "capture-cli", dependencies: ["LFCapture"]),

        // Stream C: text insertion into frontmost app
        .target(name: "LFInsert", dependencies: ["LFContracts"]),
        .executableTarget(name: "insert-cli", dependencies: ["LFInsert"]),

        // Stream D: dictionary/replacements + LLM polish (S1-mini via MLX)
        .target(
            name: "LFPolish",
            dependencies: [
                "LFContracts",
                .product(name: "MLXLLM", package: "mlx-swift-lm"),
                .product(name: "MLXLMCommon", package: "mlx-swift-lm"),
                .product(name: "MLXHuggingFace", package: "mlx-swift-lm"),
                .product(name: "HuggingFace", package: "swift-huggingface"),
                .product(name: "Transformers", package: "swift-transformers"),
            ]
        ),
        .executableTarget(name: "polish-cli", dependencies: ["LFPolish"]),

        // Stream E: menu bar app shell (real engine wired in at integration)
        .executableTarget(
            name: "LocalFlowApp",
            dependencies: [
                "LFContracts", "LFCapture", "LFInsert", "LFPolish", "LFEngine",
                .product(name: "Sparkle", package: "Sparkle"),
            ]
        ),

        .testTarget(name: "LFEngineTests", dependencies: ["LFEngine"]),
        .testTarget(name: "LFPolishTests", dependencies: ["LFPolish"]),
        .testTarget(name: "LFInsertTests", dependencies: ["LFInsert"]),
    ]
)
