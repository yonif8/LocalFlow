import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 680
    height: 560
    minimumWidth: 620
    minimumHeight: 500
    visible: false
    title: "Welcome to LocalFlow"
    modality: Qt.ApplicationModal

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 42
        spacing: 20

        Label {
            text: LocalFlowApp.models.ready ? "LocalFlow is ready" : "Private dictation, right where you type"
            font.pixelSize: 30
            font.bold: true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        Label {
            text: "Hold your push-to-talk key, speak naturally, then release. LocalFlow transcribes, polishes, and inserts your words without sending audio or text to a server."
            font.pixelSize: 16
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.82
        }

        Frame {
            Layout.fillWidth: true
            ColumnLayout {
                width: parent.width
                spacing: 10
                Label { text: "1. System access"; font.bold: true; font.pixelSize: 17 }
                Label { Layout.fillWidth: true; text: LocalFlowApp.capabilitySummary; wrapMode: Text.WordWrap }
                Label {
                    Layout.fillWidth: true
                    text: "Your operating system may ask once for microphone, global-shortcut, screen-capture, or accessibility access. LocalFlow never bypasses protected fields or secure desktops."
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            ColumnLayout {
                width: parent.width
                spacing: 10
                Label { text: "2. Local models"; font.bold: true; font.pixelSize: 17 }
                Label { text: LocalFlowApp.models.statusText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: LocalFlowApp.models.progress
                    visible: LocalFlowApp.models.busy || (!LocalFlowApp.models.ready && value > 0)
                }
                Label { text: LocalFlowApp.models.detailText; Layout.fillWidth: true; wrapMode: Text.WordWrap; opacity: 0.72 }
                RowLayout {
                    Button {
                        text: LocalFlowApp.models.busy ? "Pause" : (LocalFlowApp.models.progress > 0 ? "Resume Download" : "Download Models")
                        visible: !LocalFlowApp.models.ready
                        onClicked: LocalFlowApp.models.busy ? LocalFlowApp.models.cancel() : LocalFlowApp.models.downloadMissing()
                    }
                    Button { text: "Show Files"; flat: true; onClicked: LocalFlowApp.models.revealModels() }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        Item { Layout.fillHeight: true }
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: "Parakeet TDT v3 + S1-mini • pinned and SHA-256 verified"
                opacity: 0.55
                font.pixelSize: 12
            }
            Button {
                text: "Start LocalFlow"
                highlighted: true
                enabled: LocalFlowApp.models.ready
                onClicked: {
                    window.hide()
                    if (!LocalFlowApp.listening) LocalFlowApp.toggleListening()
                }
            }
        }
    }
}
