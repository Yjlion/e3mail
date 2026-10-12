// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// The new device's side of pairing: a code from a device that has the
// account, and a copy of it from there (ADR 0010).
Dialog {
    id: dlg
    signal joined()
    property string phase: "entering" // entering | waiting | copying | failed
    property string message
    property int percent: 0
    readonly property string account: MailApp.codeAccount(codeField.text)

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(460, parent.width - (Theme.narrow ? 16 : 96))
    title: qsTr("Copy an account from another device")
    standardButtons: Dialog.Cancel
    onOpened: { phase = "entering"; message = ""; percent = 0; codeField.text = ""; nameField.text = MailApp.defaultDeviceName(); codeField.forceActiveFocus() }
    onRejected: MailApp.cancelJoin()

    Connections {
        target: MailApp
        function onJoinWaiting() { dlg.phase = "waiting" }
        function onJoinProgress(p) { dlg.phase = "copying"; dlg.percent = p }
        function onJoinFinished(ok, message) {
            if (ok) { dlg.close(); dlg.joined() }
            else { dlg.phase = "failed"; dlg.message = message }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("On the device that has the account, open Settings, Devices, “Pair a new device”, and enter the code it shows here.")
        }
        Label { text: qsTr("Code"); color: Theme.muted }
        Field {
            id: codeField
            Layout.fillWidth: true
            enabled: dlg.phase === "entering" || dlg.phase === "failed"
            placeholderText: "E3PAIR1:…"
            font.family: "monospace"
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: codeField.text.length > 0
            color: dlg.account.length ? Theme.muted : Theme.bad
            text: dlg.account.length ? qsTr("For %1").arg(dlg.account) : qsTr("This is not a pairing code.")
        }
        Label { text: qsTr("This device’s name"); color: Theme.muted }
        Field { id: nameField; Layout.fillWidth: true; enabled: dlg.phase === "entering" || dlg.phase === "failed" }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "waiting"
            text: qsTr("Waiting for the other device to allow it…")
        }
        ProgressBar { Layout.fillWidth: true; visible: dlg.phase === "copying"; from: 0; to: 100; value: dlg.percent }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "failed"
            color: Theme.bad
            text: dlg.message
        }
        PrimaryButton {
            text: qsTr("Copy the account")
            enabled: dlg.account.length > 0 && (dlg.phase === "entering" || dlg.phase === "failed")
            onClicked: {
                dlg.message = ""
                if (MailApp.joinDevice(codeField.text, nameField.text)) dlg.phase = "waiting"
                else { dlg.phase = "failed"; dlg.message = qsTr("That code cannot be used here. Is this account already on this device?") }
            }
        }
    }
}
