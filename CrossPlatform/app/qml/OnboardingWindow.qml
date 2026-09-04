import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 680
    height: 680
    minimumWidth: 620
    minimumHeight: 500
    visible: false
    title: "Welcome to LocalFlow"
    modality: Qt.ApplicationModal

    Flickable {
        id: onboardingViewport
        anchors.fill: parent
        contentHeight: onboardingContent.implicitHeight + 84
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: onboardingContent
            x: 42
            y: 42
            width: onboardingViewport.width - 84
            spacing: 20

        Label {
            text: LocalFlowApp.models.ready && LocalFlowApp.platformReady
                ? "LocalFlow is ready" : "Private dictation, right where you type"
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
                Repeater {
                    model: LocalFlowApp.capabilities
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                            radius: 6
                            color: modelData.state === "ready" ? "#35a566"
                                : modelData.state === "permission" ? "#d28b26"
                                : modelData.state === "degraded" ? "#d28b26" : "#c84c4c"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Label { text: modelData.label; font.bold: true; Layout.fillWidth: true }
                                Label {
                                    text: modelData.state === "ready" ? "Ready"
                                        : modelData.state === "permission" ? "Asked on first use"
                                        : modelData.state === "degraded" ? "Limited" : "Action needed"
                                    opacity: 0.65
                                    font.pixelSize: 12
                                }
                            }
                            Label { Layout.fillWidth: true; text: modelData.detail; wrapMode: Text.WordWrap; opacity: 0.78 }
                            Label {
                                Layout.fillWidth: true
                                visible: modelData.remediation.length > 0 && modelData.state === "blocked"
                                text: modelData.remediation
                                wrapMode: Text.WordWrap
                                color: "#c84c4c"
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: "Permissions stay under your control. LocalFlow never bypasses password fields, protected windows, or secure desktops."
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                }
                Button { text: "Check again"; flat: true; onClicked: LocalFlowApp.refreshCapabilities() }
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
                enabled: LocalFlowApp.models.ready && LocalFlowApp.platformReady
                onClicked: {
                    window.hide()
                    if (!LocalFlowApp.listening) LocalFlowApp.toggleListening()
                }
            }
        }
        }
    }
}
