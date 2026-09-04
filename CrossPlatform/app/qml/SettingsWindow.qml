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
    readonly property var pageNames: ["General", "Dictation", "Polish", "Dictionary", "Insertion", "About", "Diagnostics"]
    readonly property var mouseTriggerValues: ["off", "middle", "side1", "side2"]

    function showPage(index) {
        page = index
        show()
        raise()
        requestActivate()
    }

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
            id: settingsViewport
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            StackLayout {
                width: settingsViewport.availableWidth
                height: Math.max(settingsViewport.availableHeight, children[currentIndex].implicitHeight)
                currentIndex: window.page

                SettingsPage {
                    title: "General"
                    Frame {
                        Layout.fillWidth: true
                        visible: LocalFlowApp.attentionRequired
                        ColumnLayout {
                            width: parent.width
                            Label { text: "Needs attention"; font.bold: true }
                            Label { Layout.fillWidth: true; text: LocalFlowApp.attentionText; wrapMode: Text.WordWrap }
                            RowLayout {
                                Button { text: "Copy transcript"; visible: LocalFlowApp.recoveryTranscript.length > 0; onClicked: LocalFlowApp.copyRecoveryTranscript() }
                                Button { text: "Dismiss"; flat: true; onClicked: LocalFlowApp.dismissAttention() }
                            }
                        }
                    }
                    Switch { text: "Launch LocalFlow at login"; checked: LocalFlowApp.settings.launchAtLogin; onToggled: LocalFlowApp.settings.launchAtLogin = checked }
                    Label { Layout.fillWidth: true; visible: LocalFlowApp.settings.launchAtLoginError.length > 0; text: LocalFlowApp.settings.launchAtLoginError; color: "#c84c4c"; wrapMode: Text.WordWrap }
                    Switch { text: "Show the recording indicator"; checked: LocalFlowApp.settings.hudEnabled; onToggled: LocalFlowApp.settings.hudEnabled = checked }
                    Label { text: "History limit: " + historySlider.value }
                    Slider { id: historySlider; from: 0; to: 50; stepSize: 1; value: LocalFlowApp.settings.historyLimit; onMoved: LocalFlowApp.settings.historyLimit = value }
                    Label { text: "This-session history"; font.bold: true }
                    ListView {
                        id: historyList
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 180)
                        visible: count > 0
                        clip: true
                        model: LocalFlowApp.history
                        spacing: 5
                        delegate: RowLayout {
                            required property int index
                            required property string display
                            width: ListView.view.width
                            Label { Layout.fillWidth: true; text: display; maximumLineCount: 2; elide: Text.ElideRight; wrapMode: Text.WordWrap }
                            Button { text: "Copy"; flat: true; onClicked: LocalFlowApp.copyHistoryItem(index) }
                        }
                    }
                    Label { text: "No history yet. History stays in memory and disappears when LocalFlow quits."; visible: historyList.count === 0; opacity: 0.65; wrapMode: Text.WordWrap }
                    Button { text: "Clear History Now"; enabled: historyList.count > 0; onClicked: LocalFlowApp.clearHistory() }
                }

                SettingsPage {
                    title: "Dictation"
                    Label { text: "Push-to-talk shortcut"; font.bold: true }
                    ComboBox { model: ["RightCtrl", "RightAlt", "F8", "F9"]; currentIndex: Math.max(0, model.indexOf(LocalFlowApp.settings.hotkey)); onActivated: LocalFlowApp.settings.hotkey = currentText }
                    Label { text: "Second mouse trigger"; font.bold: true }
                    ComboBox {
                        model: ["Off", "Middle button", "Side button 1", "Side button 2"]
                        currentIndex: Math.max(0, window.mouseTriggerValues.indexOf(LocalFlowApp.settings.mouseTrigger))
                        onActivated: LocalFlowApp.settings.mouseTrigger = window.mouseTriggerValues[currentIndex]
                    }
                    Label {
                        Layout.fillWidth: true
                        text: Qt.platform.os === "linux"
                            ? "Mouse triggers work in X11 sessions. Wayland does not expose arbitrary global mouse buttons, so the keyboard shortcut remains active there."
                            : "A middle or side button works as a second push-to-talk trigger."
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                    }
                    Label { text: "Hold threshold: " + threshold.value + " ms" }
                    Slider { id: threshold; from: 100; to: 1000; stepSize: 50; value: LocalFlowApp.settings.holdThresholdMs; onMoved: LocalFlowApp.settings.holdThresholdMs = value }
                    Switch { text: "Lower other audio while dictating"; checked: LocalFlowApp.settings.duckAudio; onToggled: LocalFlowApp.settings.duckAudio = checked }
                    Switch { text: "Keep microphone warm between dictations"; checked: LocalFlowApp.settings.keepMicWarm; onToggled: LocalFlowApp.settings.keepMicWarm = checked }
                    Label { text: "Microphone"; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            model: LocalFlowApp.microphones
                            textRole: "name"
                            valueRole: "id"
                            currentIndex: {
                                for (let i = 0; i < count; ++i) {
                                    if (valueAt(i) === LocalFlowApp.settings.microphoneId) return i
                                }
                                return 0
                            }
                            onActivated: LocalFlowApp.settings.microphoneId = currentValue
                        }
                        Button { text: "Refresh"; onClicked: LocalFlowApp.refreshMicrophones() }
                    }
                    Label { text: "If the selected microphone is disconnected, LocalFlow automatically uses the system default."; wrapMode: Text.WordWrap; opacity: 0.7 }
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
                    Label { text: "Personal replacements"; font.bold: true; font.pixelSize: 17 }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField { id: spokenField; Layout.fillWidth: true; placeholderText: "What you say" }
                        Label { text: "→" }
                        TextField { id: writtenField; Layout.fillWidth: true; placeholderText: "What LocalFlow writes" }
                        Button {
                            text: "Add"
                            enabled: spokenField.text.trim().length > 0
                                     && writtenField.text.trim().length > 0
                            onClicked: {
                                if (LocalFlowApp.settings.dictionary.addRule(spokenField.text, writtenField.text)) {
                                    spokenField.clear()
                                    writtenField.clear()
                                }
                            }
                        }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 190)
                        clip: true
                        model: LocalFlowApp.settings.dictionary
                        spacing: 5
                        delegate: RowLayout {
                            required property int index
                            required property string spoken
                            required property string written
                            width: ListView.view.width
                            TextField {
                                id: spokenEdit
                                Layout.fillWidth: true
                                text: spoken
                                onEditingFinished: {
                                    if (!LocalFlowApp.settings.dictionary.updateRule(index, text, writtenEdit.text)) {
                                        text = spoken
                                    }
                                }
                            }
                            Label { text: "→" }
                            TextField {
                                id: writtenEdit
                                Layout.fillWidth: true
                                text: written
                                onEditingFinished: {
                                    if (!LocalFlowApp.settings.dictionary.updateRule(index, spokenEdit.text, text)) {
                                        text = written
                                    }
                                }
                            }
                            Button { text: "Remove"; flat: true; onClicked: LocalFlowApp.settings.dictionary.removeRule(index) }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: LocalFlowApp.settings.dictionary.lastError.length > 0
                        text: LocalFlowApp.settings.dictionary.lastError
                        color: "#c84c4c"
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "Learned terminology (" + LocalFlowApp.learnedTerms.count + "/500)"; font.bold: true; font.pixelSize: 17; Layout.fillWidth: true }
                        Button { text: "Undo"; flat: true; enabled: LocalFlowApp.learnedTerms.canUndo; onClicked: LocalFlowApp.learnedTerms.undoLastRemoval() }
                        Button { text: "Clear"; flat: true; enabled: LocalFlowApp.learnedTerms.count > 0; onClicked: LocalFlowApp.learnedTerms.clearTerms() }
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(contentHeight, 190)
                        visible: count > 0
                        clip: true
                        model: LocalFlowApp.learnedTerms
                        spacing: 5
                        delegate: RowLayout {
                            required property int index
                            required property string canonical
                            required property var aliases
                            required property var useCount
                            width: ListView.view.width
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label { Layout.fillWidth: true; text: canonical; font.bold: true; elide: Text.ElideRight }
                                Label { Layout.fillWidth: true; text: aliases.length > 0 ? "Heard as: " + aliases.join(", ") + " • used " + useCount + "×" : "Used " + useCount + "×"; opacity: 0.65; elide: Text.ElideRight }
                            }
                            Button { text: "Remove"; flat: true; onClicked: LocalFlowApp.learnedTerms.removeTerm(index) }
                        }
                    }
                    Label { Layout.fillWidth: true; visible: LocalFlowApp.learnedTerms.lastError.length > 0; text: LocalFlowApp.learnedTerms.lastError; color: "#c84c4c"; wrapMode: Text.WordWrap }
                    Label { text: "Personal replacements and learned terminology are stored locally. The bank is capped at 500 terms and 10 aliases per term. Screen images and extracted text are never uploaded."; wrapMode: Text.WordWrap; opacity: 0.75 }
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
                    Label { Layout.fillWidth: true; visible: LocalFlowApp.updates.statusText.length > 0; text: LocalFlowApp.updates.statusText; font.bold: true; wrapMode: Text.WordWrap }
                    Label { Layout.fillWidth: true; visible: LocalFlowApp.updates.detailText.length > 0; text: LocalFlowApp.updates.detailText; opacity: 0.72; wrapMode: Text.WordWrap }
                    ProgressBar { Layout.fillWidth: true; visible: LocalFlowApp.updates.busy && LocalFlowApp.updates.progress > 0; from: 0; to: 1; value: LocalFlowApp.updates.progress }
                    RowLayout {
                        Button { text: LocalFlowApp.updates.busy ? "Working…" : "Check for Updates…"; enabled: !LocalFlowApp.updates.busy; visible: !LocalFlowApp.updates.downloadAvailable && !LocalFlowApp.updates.readyToInstall; onClicked: LocalFlowApp.checkForUpdates() }
                        Button { text: "Download Update"; highlighted: true; visible: LocalFlowApp.updates.downloadAvailable; enabled: !LocalFlowApp.updates.busy; onClicked: LocalFlowApp.updates.downloadUpdate() }
                        Button { text: "Update LocalFlow…"; highlighted: true; visible: LocalFlowApp.updates.readyToInstall; enabled: !LocalFlowApp.updates.busy; onClicked: LocalFlowApp.updates.installUpdate() }
                        Button { text: "Dismiss"; flat: true; visible: LocalFlowApp.updates.statusText.length > 0 && !LocalFlowApp.updates.busy; onClicked: LocalFlowApp.updates.dismissStatus() }
                    }
                }

                SettingsPage {
                    title: "Diagnostics"
                    Label {
                        Layout.fillWidth: true
                        text: "A content-free system report for troubleshooting. Review it before sharing."
                        wrapMode: Text.WordWrap
                        opacity: 0.75
                    }
                    Frame {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 320
                        ScrollView {
                            anchors.fill: parent
                            TextArea {
                                text: LocalFlowApp.diagnosticsReport
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                            }
                        }
                    }
                    RowLayout {
                        Button { text: "Check again"; onClicked: LocalFlowApp.refreshCapabilities() }
                        Button { text: "Copy report"; onClicked: LocalFlowApp.copyDiagnostics() }
                        Button { text: "Open GitHub issue…"; highlighted: true; onClicked: LocalFlowApp.openIssue() }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: "Never includes dictated text, visible screen text, screenshots, audio, history, paths, usernames, or device identifiers."
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                    }
                }
            }
        }
    }

    component SettingsPage: Item {
        required property string title
        default property alias pageContent: content.data
        implicitHeight: content.implicitHeight + 68
        ColumnLayout {
            id: content
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 34
            spacing: 16
            Label { text: title; font.pixelSize: 26; font.bold: true; Layout.bottomMargin: 8 }
        }
    }
}
