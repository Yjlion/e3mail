// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Pairing a new device with this account: show a code, let the person
// allow the device that uses it, then send it a copy (ADR 0010).
Dialog {
    id: dlg
    property string code
    property string requester
    property string phase: "waiting" // waiting | asking | copying | done | failed
    property string message
    property int percent: 0

    function begin() {
        phase = "waiting"; requester = ""; message = ""; percent = 0
        code = MailApp.startPairing()
        if (!code.length) { phase = "failed"; message = qsTr("This device could not start its connection to other devices.") }
        open()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(460, parent.width - (Theme.narrow ? 16 : 96))
    title: qsTr("Pair a new device")
    standardButtons: dlg.phase === "done" || dlg.phase === "failed" ? Dialog.Close : Dialog.Cancel
    onRejected: MailApp.cancelPairing()
    onClosed: MailApp.cancelPairing()

    Connections {
        target: MailApp
        function onPairingRequested(name) { dlg.requester = name; dlg.phase = "asking" }
        function onPairingProgress(p) { dlg.percent = p }
        function onPairingFinished(ok, message) { dlg.phase = ok ? "done" : "failed"; dlg.message = message }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "waiting"
            text: qsTr("On the new device, choose “Copy an account from another device” when adding a mailbox, and enter this code. It works once, for ten minutes.")
        }
        Image {
            Layout.alignment: Qt.AlignHCenter
            visible: dlg.phase === "waiting" && dlg.code.length > 0
            source: dlg.code.length ? "image://qr/" + encodeURIComponent(dlg.code) : ""
            sourceSize: Qt.size(260, 260)
            smooth: false
            Accessible.name: qsTr("Pairing code as a QR code")
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dlg.phase === "waiting" && dlg.code.length > 0
            TextField {
                id: codeField
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: dlg.code
                font.family: "monospace"
                font.pixelSize: Theme.fontSmall
            }
            Button { text: qsTr("Copy"); onClicked: { codeField.selectAll(); codeField.copy(); codeField.deselect() } }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "asking"
            font.weight: Font.DemiBold
            text: qsTr("“%1” wants to become a device of this account. It will get a copy of all your mail, your contacts, your key and your password.").arg(dlg.requester)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "asking"
            color: Theme.muted
            text: qsTr("Allow it only if you are setting up that device yourself, now.")
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            visible: dlg.phase === "asking"
            PrimaryButton { text: qsTr("Allow"); onClicked: { dlg.phase = "copying"; MailApp.allowPairing() } }
            Button { text: qsTr("Refuse"); flat: true; onClicked: MailApp.denyPairing() }
        }
        Label { visible: dlg.phase === "copying"; text: qsTr("Sending a copy to “%1”…").arg(dlg.requester) }
        ProgressBar { Layout.fillWidth: true; visible: dlg.phase === "copying"; from: 0; to: 100; value: dlg.percent }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "done" || dlg.phase === "failed"
            color: dlg.phase === "done" ? Theme.good : Theme.bad
            text: dlg.phase === "done" ? qsTr("“%1” is now a device of this account.").arg(dlg.requester) : dlg.message
        }
    }
}
