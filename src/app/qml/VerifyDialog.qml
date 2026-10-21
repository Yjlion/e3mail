// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Verifying someone in person (SecureJoin, ADR 0021): show this account's
// code for them to scan, or scan or paste theirs. Works with Delta Chat too.
Dialog {
    id: dlg
    property string invite
    property string phase: "showing" // showing | scanning | waiting | done | failed
    property string peer             // the address being verified
    property string message
    signal verified(string addr)

    function begin() {
        invite = MailApp.secureJoinInvite()
        phase = "showing"; peer = ""; message = ""; theirCode.text = ""
        open()
    }
    // For screenshots (--page verify-scan).
    function scan() { phase = "scanning" }
    function start(text) {
        const err = MailApp.secureJoin(text.trim())
        if (err.length) { message = err; return }
        peer = MailApp.secureJoinAddress(text.trim())
        message = ""
        phase = "waiting"
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(480, parent.width - (Theme.narrow ? 16 : 96))
    title: qsTr("Verify in person")
    standardButtons: Dialog.Close

    // The answer arrives by mail; look every second while waiting.
    Timer {
        interval: 1000
        repeat: true
        running: dlg.visible && dlg.phase === "waiting"
        onTriggered: {
            const s = MailApp.secureJoinState(dlg.peer)
            if (s === "done") { dlg.phase = "done"; dlg.verified(dlg.peer) }
            else if (s === "failed") dlg.phase = "failed"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "showing"
            text: qsTr("Meet, or call on video. Let them scan this code with e3mail or Delta Chat, or scan theirs. Each of you then has the other’s key verified.")
        }
        Image {
            Layout.alignment: Qt.AlignHCenter
            visible: dlg.phase === "showing" && dlg.invite.length > 0
            source: dlg.invite.length ? "image://qr/" + encodeURIComponent(dlg.invite) : ""
            sourceSize: Qt.size(240, 240)
            smooth: false
            Accessible.name: qsTr("Your code as a QR code")
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dlg.phase === "showing"
            TextField {
                id: inviteField
                Layout.fillWidth: true
                readOnly: true
                selectByMouse: true
                text: dlg.invite
                font.family: "monospace"
                font.pixelSize: Theme.fontSmall
            }
            Button { text: qsTr("Copy"); onClicked: { inviteField.selectAll(); inviteField.copy(); inviteField.deselect() } }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "showing"
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
            text: qsTr("Anyone who sees this code can verify themselves to you. If the wrong person saw it, make a new one.")
        }
        Button {
            visible: dlg.phase === "showing"
            flat: true
            text: qsTr("New code")
            onClicked: dlg.invite = MailApp.renewSecureJoinInvite()
        }

        Loader {
            Layout.fillWidth: true
            active: dlg.phase === "scanning"
            visible: active
            source: "ScanView.qml"
            onLoaded: {
                item.prefixes = ["OPENPGP4FPR:", "https://i.delta.chat"]
                item.hint = qsTr("Point the camera at the other person’s code.")
                item.found.connect(function(code) { dlg.phase = "showing"; theirCode.text = code; dlg.start(code) })
                item.cancelled.connect(function() { dlg.phase = "showing"; theirCode.forceActiveFocus() })
            }
        }

        Label {
            visible: dlg.phase === "showing"
            Layout.topMargin: 6
            text: qsTr("Their code")
            color: Theme.muted
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dlg.phase === "showing"
            Field {
                id: theirCode
                Layout.fillWidth: true
                placeholderText: "OPENPGP4FPR:…"
                onAccepted: if (text.length) dlg.start(text)
            }
            Button {
                visible: MailApp.scanAvailable
                text: qsTr("Scan")
                onClicked: dlg.phase = "scanning"
            }
            PrimaryButton {
                text: qsTr("Verify")
                enabled: MailApp.isSecureJoinInvite(theirCode.text)
                onClicked: dlg.start(theirCode.text)
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.message.length > 0
            color: Theme.bad
            text: dlg.message
        }

        BusyIndicator { Layout.alignment: Qt.AlignHCenter; visible: dlg.phase === "waiting"; running: visible }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            visible: dlg.phase === "waiting"
            text: qsTr("Exchanging keys with %1 by mail. This takes as long as your mail does; you can close this and come back.").arg(dlg.peer)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: dlg.phase === "done" || dlg.phase === "failed"
            color: dlg.phase === "done" ? Theme.good : Theme.bad
            text: dlg.phase === "done" ? qsTr("%1 is verified.").arg(dlg.peer)
                : qsTr("%1 did not answer within a week. Try again when you can both check your mail.").arg(dlg.peer)
        }
    }
}
