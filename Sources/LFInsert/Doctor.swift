import AppKit

/// Snapshot of everything relevant to "why isn't insertion working?".
public struct InsertDoctorReport: Sendable {
    public let accessibilityGranted: Bool
    public let secureInputEnabled: Bool
    public let frontmostAppName: String?
    public let frontmostBundleID: String?
    public let focusedElementRole: String?
    public let focusedElementSubrole: String?
    public let selectedTextSettable: Bool
    /// Raw AXError code when the focused element could not be obtained.
    public let focusedElementLookupError: Int32?

    /// Gathers the report. Call while the target app is frontmost.
    @MainActor
    public static func gather() -> InsertDoctorReport {
        let frontmost = NSWorkspace.shared.frontmostApplication
        let info = AXInsertion.focusedElementInfo()
        return InsertDoctorReport(
            accessibilityGranted: InsertPermissions.accessibilityGranted(),
            secureInputEnabled: InsertPermissions.secureInputEnabled,
            frontmostAppName: frontmost?.localizedName,
            frontmostBundleID: frontmost?.bundleIdentifier,
            focusedElementRole: info.role,
            focusedElementSubrole: info.subrole,
            selectedTextSettable: info.selectedTextSettable,
            focusedElementLookupError: info.lookupError?.rawValue
        )
    }
}
