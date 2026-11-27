import AppKit
import CoreGraphics
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

/// Whole-display screenshot for "Send to WhatsApp". Captures the display under
/// the mouse pointer (where the user is looking), leaves LocalFlow's own HUD
/// out of the picture, and encodes PNG locally. Nothing leaves the device here.
enum ScreenSnapshot {
    enum Failure: Error, CustomStringConvertible {
        case permissionMissing
        case noDisplay
        case encodingFailed

        var description: String {
            switch self {
            case .permissionMissing: return "Screen Recording permission is needed for the screenshot."
            case .noDisplay: return "Couldn't find a display to capture."
            case .encodingFailed: return "Couldn't encode the screenshot."
            }
        }
    }

    static func captureDisplayPNG() async throws -> Data {
        guard ScreenOCR.hasPermission else { throw Failure.permissionMissing }
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { $0.frame.contains(mouse) } ?? NSScreen.main
        let screenNumber = screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID
        guard let display = content.displays.first(where: { $0.displayID == screenNumber })
            ?? content.displays.first else { throw Failure.noDisplay }

        let ownApps = content.applications.filter { $0.processID == ProcessInfo.processInfo.processIdentifier }
        let filter = SCContentFilter(display: display, excludingApplications: ownApps, exceptingWindows: [])
        let configuration = SCStreamConfiguration()
        let scale = screen?.backingScaleFactor ?? 2
        configuration.width = Int(CGFloat(display.width) * scale)
        configuration.height = Int(CGFloat(display.height) * scale)
        configuration.showsCursor = false

        let image = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: configuration)
        return try encodePNG(image)
    }

    static func encodePNG(_ image: CGImage) throws -> Data {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(
            data, UTType.png.identifier as CFString, 1, nil) else { throw Failure.encodingFailed }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else { throw Failure.encodingFailed }
        return data as Data
    }
}
