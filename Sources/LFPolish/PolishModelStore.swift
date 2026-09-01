import Foundation

/// Canonical on-disk location and download-progress surface for the S1-mini
/// polish model. Mirrors `LFEngine.EngineModelLocations` for the ASR models:
/// everything lives under `~/Library/Application Support/LocalFlow/s1-mini/`
/// (a Hugging Face `HubCache` layout: `models--<owner>--<repo>/{refs,blobs,
/// snapshots}`). The `LOCALFLOW_MODELS_ROOT` environment variable overrides
/// the root — a debug/testing hook, never needed in normal operation.
public enum PolishModelStore {
    /// Byte-weighted download progress for the polish model.
    public struct Progress: Sendable {
        public let fractionCompleted: Double
        public let completedBytes: Int64
        public let totalBytes: Int64
    }

    /// Root directory holding every LocalFlow model subdirectory.
    public static var rootDirectory: URL {
        if let override = ProcessInfo.processInfo.environment["LOCALFLOW_MODELS_ROOT"],
           !override.isEmpty {
            return URL(
                fileURLWithPath: NSString(string: override).expandingTildeInPath,
                isDirectory: true)
        }
        let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return appSupport.appendingPathComponent("LocalFlow", isDirectory: true)
    }

    /// `HubCache` directory for the polish model.
    public static var cacheDirectory: URL {
        rootDirectory.appendingPathComponent("s1-mini", isDirectory: true)
    }

    /// The model's repository directory inside `cacheDirectory`
    /// (`models--mlx-community--S1-mini-MLX-8bit`).
    public static var repoDirectory: URL {
        cacheDirectory.appendingPathComponent(
            "models--" + S1MiniBackend.modelID.replacingOccurrences(of: "/", with: "--"),
            isDirectory: true)
    }

    /// True when a complete local snapshot of the polish model exists (a ref
    /// plus a snapshot whose weight symlink resolves to a present blob).
    public static var isModelDownloaded: Bool {
        let fm = FileManager.default
        let repo = repoDirectory
        guard fm.fileExists(atPath: repo.appendingPathComponent("refs/main").path) else {
            return false
        }
        let snapshotsDir = repo.appendingPathComponent("snapshots", isDirectory: true)
        guard let snapshots = try? fm.contentsOfDirectory(
            at: snapshotsDir, includingPropertiesForKeys: nil) else {
            return false
        }
        // fileExists(atPath:) follows the relative snapshot symlinks, so this
        // also verifies the backing blob is present.
        return snapshots.contains { snapshot in
            fm.fileExists(atPath: snapshot.appendingPathComponent("model.safetensors").path)
        }
    }

    // MARK: - Progress surface

    private static let lock = NSLock()
    nonisolated(unsafe) private static var _progressHandler:
        (@Sendable (Progress) -> Void)?

    /// Called during an S1-mini download with byte-weighted progress
    /// (mlx-swift-lm delivers updates on the main actor, resampled ~100 ms;
    /// a cache hit delivers a single 1/1 update). Set this before the first
    /// prewarm/polish to observe first-run downloads.
    public static var progressHandler: (@Sendable (Progress) -> Void)? {
        get {
            lock.lock()
            defer { lock.unlock() }
            return _progressHandler
        }
        set {
            lock.lock()
            defer { lock.unlock() }
            _progressHandler = newValue
        }
    }

    static func report(_ progress: Foundation.Progress) {
        progressHandler?(Progress(
            fractionCompleted: progress.fractionCompleted,
            completedBytes: progress.completedUnitCount,
            totalBytes: progress.totalUnitCount))
    }
}
