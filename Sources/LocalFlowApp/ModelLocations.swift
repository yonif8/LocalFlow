import Foundation
import Observation
import os
import LFEngine
import LFPolish

/// Model housekeeping: one-time cleanup of caches from removed engines and
/// the legacy S1-mini migration.
///
/// Parakeet (speech) lives in FluidAudio's own Application Support cache;
/// S1-mini (polish) lives in `~/Library/Application Support/LocalFlow/s1-mini`.
/// The Granite engine was removed in 1.1.0 — its caches (~1.1 GB) are deleted
/// on first launch after the update to reclaim disk space.
enum ModelLocations {
    private static let logger = Logger(subsystem: "com.localflow.app", category: "models")

    /// True when LOCALFLOW_MODELS_ROOT redirects the model root (debug hook).
    static var isOverridden: Bool {
        !(ProcessInfo.processInfo.environment["LOCALFLOW_MODELS_ROOT"] ?? "").isEmpty
    }

    static func migrateLegacyCachesIfNeeded() {
        guard !isOverridden else { return }
        let fm = FileManager.default
        let home = fm.homeDirectoryForCurrentUser
        let appSupport = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("LocalFlow", isDirectory: true)

        // S1-mini legacy: ~/.cache/huggingface/hub/models--… → s1-mini/models--…
        let repoName = PolishModelStore.repoDirectory.lastPathComponent
        moveIfNeeded(
            from: home.appendingPathComponent(
                ".cache/huggingface/hub/\(repoName)", isDirectory: true),
            to: PolishModelStore.repoDirectory)

        // Granite engine removed in 1.1.0: reclaim its caches.
        for stale in [
            appSupport.appendingPathComponent("granite", isDirectory: true),
            appSupport.appendingPathComponent("hf-cache", isDirectory: true),
            home.appendingPathComponent("Documents/huggingface/models/iky1e", isDirectory: true),
        ] where fm.fileExists(atPath: stale.path) {
            do {
                try fm.removeItem(at: stale)
                logger.info("removed stale model cache \(stale.path, privacy: .public)")
            } catch {
                logger.error("could not remove \(stale.path, privacy: .public): \(String(describing: error), privacy: .public)")
            }
        }
    }

    private static func moveIfNeeded(from source: URL, to destination: URL) {
        let fm = FileManager.default
        guard fm.fileExists(atPath: source.path) else { return }
        guard !fm.fileExists(atPath: destination.path) else { return }
        do {
            try fm.createDirectory(
                at: destination.deletingLastPathComponent(),
                withIntermediateDirectories: true)
            try fm.moveItem(at: source, to: destination)
            logger.info("migrated model dir \(source.path, privacy: .public) → \(destination.path, privacy: .public)")
        } catch {
            // Fail open: the model re-downloads into the new location.
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
    private(set) var polish: Status = .unknown

    var allDownloaded: Bool {
        speech == .downloaded && polish == .downloaded
    }

    private init() {}

    /// Reconcile with what's on disk. Never downgrades a row that is
    /// actively reporting download progress.
    func refreshFromDisk() {
        if !speech.isDownloading {
            setStatus(
                &speech,
                to: ParakeetTranscriber.isModelDownloaded ? .downloaded : .waiting,
                name: "speech")
        }
        if !polish.isDownloading {
            setStatus(
                &polish,
                to: PolishModelStore.isModelDownloaded ? .downloaded : .waiting,
                name: "polish")
        }
    }

    func noteEngineProgress(_ progress: EngineModelProgress) {
        if progress.fractionCompleted >= 1 || progress.phase == "complete" {
            setStatus(&speech, to: .downloaded, name: "speech")
        } else {
            setStatus(
                &speech,
                to: .downloading(
                    fraction: progress.fractionCompleted,
                    completedMB: Int(progress.fractionCompleted * 600),
                    totalMB: 600),
                name: "speech")
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
