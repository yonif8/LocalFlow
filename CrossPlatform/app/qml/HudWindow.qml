import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: hud
    width: 220
    height: 48
    visible: LocalFlowApp.settings.hudEnabled && (LocalFlowApp.state !== "idle")
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
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
                text: LocalFlowApp.statusText
                color: "white"
                font.pixelSize: 13
                elide: Text.ElideRight
            }
        }
    }
}
