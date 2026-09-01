import Foundation
import Observation
import os
import LFEngine
import LFPolish

/// Where LocalFlow's models live on disk, plus the one-time migration of the
/// legacy caches into `~/Library/Application Support/LocalFlow/`.
///
/// Legacy locations (from before models were app-managed):
///   - Granite speech + punctuation: `~/Documents/huggingface/models/<owner>/<repo>`
///   - S1-mini polish model:         `~/.cache/huggingface/hub/models--mlx-community--S1-mini-MLX-8bit`
///
/// Both are safe to migrate as plain directory moves: Granite materialized
/// checkpoints are ordinary files, and the S1 HubCache snapshot symlinks are
/// relative. Only the directories LocalFlow actually uses are moved; anything
/// else in those caches is left alone.
enum ModelLocations {
    private static let logger = Logger(subsystem: "com.localflow.app", category: "models")

    /// True when LOCALFLOW_MODELS_ROOT redirects the model root (debug hook).
    static var isOverridden: Bool {
        !(ProcessInfo.processInfo.environment["LOCALFLOW_MODELS_ROOT"] ?? "").isEmpty
    }

    private static let graniteRepositories = [
        "iky1e/granite-speech-5.0-470m-turboctc-mlx-q8",
        "iky1e/punctuation-fullstop-truecase-english-mlx-q8",
    ]

    /// One-time, silent migration of the legacy model caches. Runs at app
    /// startup; a no-op when the legacy directories are gone or the new ones
    /// already exist. Never runs against a debug-overridden root.
    static func migrateLegacyCachesIfNeeded() {
        guard !isOverridden else { return }
        let home = FileManager.default.homeDirectoryForCurrentUser

        // Granite: ~/Documents/huggingface/models/<owner>/<repo> → granite/models/<owner>/<repo>
        let legacyModels = home.appendingPathComponent(
            "Documents/huggingface/models", isDirectory: true)
        let newModels = EngineModelLocations.modelsDirectory
        for repository in graniteRepositories {
            moveIfNeeded(
                from: legacyModels.appendingPathComponent(repository, isDirectory: true),
                to: newModels.appendingPathComponent(repository, isDirectory: true))
        }

        // S1-mini: ~/.cache/huggingface/hub/models--…  →  s1-mini/models--…
        let repoName = PolishModelStore.repoDirectory.lastPathComponent
        moveIfNeeded(
            from: home.appendingPathComponent(
                ".cache/huggingface/hub/\(repoName)", isDirectory: true),
            to: PolishModelStore.repoDirectory)
    }

    private static func moveIfNeeded(from source: URL, to destination: URL) {
        let fm = FileManager.default
        guard fm.fileExists(atPath: source.path) else { return }
        guard !fm.fileExists(atPath: destination.path) else {
            logger.info("migration skipped, destination exists: \(destination.path, privacy: .public)")
            return
        }
        do {
            try fm.createDirectory(
                at: destination.deletingLastPathComponent(),
                withIntermediateDirectories: true)
            try fm.moveItem(at: source, to: destination)
            logger.info("migrated model dir \(source.path, privacy: .public) → \(destination.path, privacy: .public)")
        } catch {
            // Fail open: the engine will re-download into the new location.
            logger.error("model migration failed for \(source.path, privacy: .public): \(String(describing: error), privacy: .public)")
        }
    }
}

/// Live model download/readiness state driving the onboarding "Models"
/// section. Fed by the engine/polish progress callbacks (first download)
/// and by on-disk checks (cache hits, app restarts).
@MainActor
@Observable
final class ModelSetupState {
    static let shared = ModelSetupState()
    private static let logger = Logger(subsystem: "com.localflow.app", category: "models")

    enum Status: Equatable {
        case unknown
        case waiting
        case downloading(fraction: Double, completedMB: Int, totalMB: Int?)
        case downloaded
    }

    private(set) var speech: Status = .unknown
    private(set) var punctuation: Status = .unknown
    private(set) var polish: Status = .unknown

    var allDownloaded: Bool {
        speech == .downloaded && punctuation == .downloaded && polish == .downloaded
    }

    private init() {}

    /// Reconcile with what's on disk. Never downgrades a row that is
    /// actively reporting download progress.
    func refreshFromDisk() {
        if !speech.isDownloading {
            setStatus(
                &speech,
                to: EngineModelLocations.isSpeechModelDownloaded() ? .downloaded : .waiting,
                name: "speech")
        }
        if !punctuation.isDownloading {
            setStatus(
                &punctuation,
                to: EngineModelLocations.isPunctuationModelDownloaded() ? .downloaded : .waiting,
                name: "punctuation")
        }
        if !polish.isDownloading {
            setStatus(
                &polish,
                to: PolishModelStore.isModelDownloaded ? .downloaded : .waiting,
                name: "polish")
        }
    }

    /// Route Granite progress (speech or punctuation repo) to its row.
    func noteEngineProgress(_ progress: EngineModelProgress) {
        let status = Self.status(for: progress)
        if progress.repositoryID.contains("punctuation") {
            setStatus(&punctuation, to: status, name: "punctuation")
        } else {
            setStatus(&speech, to: status, name: "speech")
        }
    }

    func notePolishProgress(_ progress: PolishModelStore.Progress) {
        if progress.fractionCompleted >= 1 {
            setStatus(&polish, to: .downloaded, name: "polish")
        } else {
            setStatus(
                &polish,
                to: .downloading(
                    fraction: progress.fractionCompleted,
                    completedMB: Int(progress.completedBytes / 1_000_000),
                    totalMB: progress.totalBytes > 0
                        ? Int(progress.totalBytes / 1_000_000) : nil),
                name: "polish")
        }
    }

    private static func status(for progress: EngineModelProgress) -> Status {
        switch progress.phase {
        case "cache_hit", "complete":
            return .downloaded
        default:
            let total = progress.estimatedTotalBytes
            return .downloading(
                fraction: progress.fractionCompleted,
                completedMB: total.map {
                    Int(progress.fractionCompleted * Double($0) / 1_000_000)
                } ?? 0,
                totalMB: total.map { Int($0 / 1_000_000) })
        }
    }

    private func setStatus(_ row: inout Status, to status: Status, name: String) {
        guard row != status else { return }
        // Log state changes for headless verification of the first-run
        // download UI — but for downloading→downloading ticks only when the
        // 10%-bucket changes, not every 100 ms progress event.
        switch (row, status) {
        case (.downloading(let old, _, _), .downloading(let new, _, _))
            where Int(old * 10) == Int(new * 10):
            break
        default:
            Self.logger.info("model \(name, privacy: .public): \(String(describing: status), privacy: .public)")
        }
        row = status
    }
}

extension ModelSetupState.Status {
    var isDownloading: Bool {
        if case .downloading = self { return true }
        return false
    }
}
