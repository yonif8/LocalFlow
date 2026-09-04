import QtQuick
import QtQuick.Controls
import LocalFlow

QtObject {
    id: root

    property SettingsWindow settingsWindow: SettingsWindow {}
    property HudWindow hudWindow: HudWindow {}
    property OnboardingWindow onboardingWindow: OnboardingWindow {}

    property Connections appConnections: Connections {
        target: LocalFlowApp
        function onSettingsRequested() {
            root.settingsWindow.show()
            root.settingsWindow.raise()
            root.settingsWindow.requestActivate()
        }
        function onSettingsDismissed() { root.settingsWindow.hide() }
        function onOnboardingRequested() {
            root.onboardingWindow.show()
            root.onboardingWindow.raise()
            root.onboardingWindow.requestActivate()
        }
    }
}
