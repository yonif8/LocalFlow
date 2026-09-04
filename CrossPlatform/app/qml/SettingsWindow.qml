import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 860
    height: 620
    minimumWidth: 760
    minimumHeight: 540
    visible: false
    title: "LocalFlow Settings"
    color: palette.window

    property int page: 0
    readonly property var pageNames: ["General", "Dictation", "Polish", "Dictionary", "Insertion", "About"]

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 190
            color: window.palette.alternateBase

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 6

                Label {
                    text: "LocalFlow"
                    font.pixelSize: 21
                    font.bold: true
                    Layout.leftMargin: 10
                    Layout.topMargin: 8
                    Layout.bottomMargin: 10
                }
                Repeater {
                    model: window.pageNames
                    delegate: Button {
                        required property int index
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        flat: index !== window.page
                        highlighted: index === window.page
                        onClicked: window.page = index
                    }
                }
                Item { Layout.fillHeight: true }
                Label {
                    Layout.fillWidth: true
                    text: LocalFlowApp.statusText
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    font.pixelSize: 12
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth

            StackLayout {
                width: parent.width
                currentIndex: window.page

                SettingsPage {
                    title: "General"
                    Switch { text: "Launch LocalFlow at login"; checked: LocalFlowApp.settings.launchAtLogin; onToggled: LocalFlowApp.settings.launchAtLogin = checked }
                    Switch { text: "Show the recording indicator"; checked: LocalFlowApp.settings.hudEnabled; onToggled: LocalFlowApp.settings.hudEnabled = checked }
                    Label { text: "History limit: " + historySlider.value }
                    Slider { id: historySlider; from: 0; to: 50; stepSize: 1; value: LocalFlowApp.settings.historyLimit; onMoved: LocalFlowApp.settings.historyLimit = value }
                    Button { text: "Clear History Now"; onClicked: LocalFlowApp.clearHistory() }
                }

                SettingsPage {
                    title: "Dictation"
                    Label { text: "Push-to-talk shortcut"; font.bold: true }
                    ComboBox { model: ["RightCtrl", "RightAlt", "F8", "Ctrl+Space"]; currentIndex: Math.max(0, model.indexOf(LocalFlowApp.settings.hotkey)); onActivated: LocalFlowApp.settings.hotkey = currentText }
                    Label { text: "Hold threshold: " + threshold.value + " ms" }
                    Slider { id: threshold; from: 100; to: 1000; stepSize: 50; value: LocalFlowApp.settings.holdThresholdMs; onMoved: LocalFlowApp.settings.holdThresholdMs = value }
                    Switch { text: "Lower other audio while dictating"; checked: LocalFlowApp.settings.duckAudio; onToggled: LocalFlowApp.settings.duckAudio = checked }
                    Switch { text: "Keep microphone warm between dictations"; checked: LocalFlowApp.settings.keepMicWarm; onToggled: LocalFlowApp.settings.keepMicWarm = checked }
                    Label { text: "Microphone selection will list devices reported by the operating system."; wrapMode: Text.WordWrap; opacity: 0.7 }
                }

                SettingsPage {
                    title: "Polish"
                    Switch { text: "Polish dictated text with S1-mini"; checked: LocalFlowApp.settings.polishEnabled; onToggled: LocalFlowApp.settings.polishEnabled = checked }
                    Label { text: "Writing style"; font.bold: true }
                    ComboBox { model: ["auto", "casual", "neutral"]; currentIndex: model.indexOf(LocalFlowApp.settings.polishTone); onActivated: LocalFlowApp.settings.polishTone = currentText }
                    Label { text: "Timeout: " + timeout.value / 1000 + " seconds" }
                    Slider { id: timeout; from: 500; to: 5000; stepSize: 100; value: LocalFlowApp.settings.polishTimeoutMs; onMoved: LocalFlowApp.settings.polishTimeoutMs = value }
                    Label { text: "Maximum input: " + maxChars.value + " characters" }
                    Slider { id: maxChars; from: 100; to: 4000; stepSize: 100; value: LocalFlowApp.settings.polishMaxCharacters; onMoved: LocalFlowApp.settings.polishMaxCharacters = value }
                }

                SettingsPage {
                    title: "Dictionary"
                    Switch { text: "Learn terminology from visible screen text"; checked: LocalFlowApp.settings.screenTerminologyEnabled; onToggled: LocalFlowApp.settings.screenTerminologyEnabled = checked }
                    Switch { text: "Recognize spoken punctuation commands"; checked: LocalFlowApp.settings.spokenPunctuationEnabled; onToggled: LocalFlowApp.settings.spokenPunctuationEnabled = checked }
                    Label { text: "Personal replacements and learned terminology are stored locally. Screen images and extracted text are never uploaded."; wrapMode: Text.WordWrap; opacity: 0.75 }
                    Button { text: "Manage Personal Dictionary…" }
                    Button { text: "Review Learned Terminology…" }
                }

                SettingsPage {
                    title: "Insertion"
                    Label { text: "Insertion method"; font.bold: true }
                    ComboBox { model: ["auto", "paste", "type"]; currentIndex: model.indexOf(LocalFlowApp.settings.insertionMethod); onActivated: LocalFlowApp.settings.insertionMethod = currentText }
                    Label { text: "Clipboard restore delay: " + restoreDelay.value + " ms" }
                    Slider { id: restoreDelay; from: 50; to: 1500; stepSize: 50; value: LocalFlowApp.settings.clipboardRestoreDelayMs; onMoved: LocalFlowApp.settings.clipboardRestoreDelayMs = value }
                    Label { text: "Automatic insertion uses the safest direct method available, then falls back to a protected clipboard transaction."; wrapMode: Text.WordWrap; opacity: 0.75 }
                }

                SettingsPage {
                    title: "About"
                    Label { text: "LocalFlow " + Qt.application.version; font.pixelSize: 18; font.bold: true }
                    Label { text: "Fully local push-to-talk dictation for Windows and Linux." }
                    Label { text: "Speech recognition: NVIDIA Parakeet TDT v3\nPolish: S1-mini by Superwhisper\nNo accounts, cloud processing, or telemetry."; wrapMode: Text.WordWrap }
                    Label { text: LocalFlowApp.capabilitySummary; wrapMode: Text.WordWrap; opacity: 0.7 }
                    Button { text: "Check for Updates…"; onClicked: LocalFlowApp.checkForUpdates() }
                }
            }
        }
    }

    component SettingsPage: Item {
        required property string title
        default property alias pageContent: content.data
        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 34
            spacing: 16
            Label { text: title; font.pixelSize: 26; font.bold: true; Layout.bottomMargin: 8 }
        }
    }
}
