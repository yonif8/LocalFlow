import AppKit
import CoreGraphics
import Foundation
import LFPolish
import ScreenCaptureKit
import Vision

struct ScreenOCRResult: Sendable {
    let terms: [String]
    let recognizedLines: Int
    let elapsed: Duration
}

/// Captures the frontmost app's active window and recognizes all visible text
/// locally. OCR is the primary screen-context source because many apps render
/// text without publishing it through Accessibility.
enum ScreenOCR {
    static var hasPermission: Bool { CGPreflightScreenCaptureAccess() }

    @discardableResult
    static func requestPermission() -> Bool {
        CGRequestScreenCaptureAccess()
    }

    static func capture(processID: pid_t) async throws -> ScreenOCRResult {
        let clock = ContinuousClock()
        let start = clock.now
        let content = try await SCShareableContent.current
        guard let window = content.windows
            .filter({
                $0.owningApplication?.processID == processID
                    && $0.isOnScreen
                    && $0.windowLayer == 0
                    && $0.frame.width >= 200
                    && $0.frame.height >= 120
            })
            .max(by: { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height })
        else {
            return .init(terms: [], recognizedLines: 0, elapsed: clock.now - start)
        }

        let filter = SCContentFilter(desktopIndependentWindow: window)
        let configuration = SCStreamConfiguration()
        let pixelWidth = min(max(window.frame.width * 2, 1), 1_800)
        let scale = pixelWidth / max(window.frame.width, 1)
        configuration.width = Int(pixelWidth)
        configuration.height = Int(max(window.frame.height * scale, 1))
        configuration.showsCursor = false

        let image = try await SCScreenshotManager.captureImage(
            contentFilter: filter, configuration: configuration)
        let strings = try recognizeText(in: image)
        return .init(
            terms: ScreenTermExtractor.extract(from: strings),
            recognizedLines: strings.count,
            elapsed: clock.now - start)
    }

    private static func recognizeText(in image: CGImage) throws -> [String] {
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        // Preserve spellings such as LFPolish, PostgreSQL and filenames instead
        // of allowing language correction to turn them into ordinary words.
        request.usesLanguageCorrection = false
        request.recognitionLanguages = ["en-US"]
        request.minimumTextHeight = 0.006

        let handler = VNImageRequestHandler(cgImage: image, options: [:])
        try handler.perform([request])
        return (request.results ?? []).compactMap {
            $0.topCandidates(1).first?.string
        }
    }
}
