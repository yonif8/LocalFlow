import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: hud
    width: LocalFlowApp.state === "error" ? 440 : (LocalFlowApp.state === "recording" ? 330 : 260)
    height: LocalFlowApp.state === "error" ? 112 : 48
    x: Screen.virtualX + Math.round((Screen.width - width) / 2)
    y: Screen.virtualY + Screen.height - height - 64
    visible: LocalFlowApp.settings.hudEnabled && (LocalFlowApp.state !== "idle")
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | (LocalFlowApp.state === "error" ? 0 : Qt.WindowDoesNotAcceptFocus)
    color: "transparent"

    Rectangle {
        anchors.fill: parent
        radius: 16
        color: "#e61d2025"
        border.color: "#33ffffff"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 12

            Row {
                visible: LocalFlowApp.state !== "error"
                spacing: 3
                Repeater {
                    model: 5
                    Rectangle {
                        width: 3
                        radius: 2
                        color: "#63d6ff"
                        height: 6 + Math.max(0.08, LocalFlowApp.inputLevel) * (8 + index * 2)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: LocalFlowApp.state === "error" ? LocalFlowApp.attentionText : LocalFlowApp.statusText
                color: "white"
                font.pixelSize: 13
                elide: LocalFlowApp.state === "error" ? Text.ElideNone : Text.ElideRight
                wrapMode: LocalFlowApp.state === "error" ? Text.WordWrap : Text.NoWrap
            }
            Button {
                visible: LocalFlowApp.state === "recording"
                text: "Cancel"
                onClicked: LocalFlowApp.cancelDictation()
            }
            ColumnLayout {
                visible: LocalFlowApp.state === "error"
                Button {
                    text: "Copy"
                    visible: LocalFlowApp.recoveryTranscript.length > 0
                    onClicked: LocalFlowApp.copyRecoveryTranscript()
                }
                Button { text: "Dismiss"; onClicked: LocalFlowApp.dismissAttention() }
            }
        }
    }
}
