// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Said once, before any password is asked for. Escape and clicking outside do
// not dismiss it.
Dialog {
    id: dlg
    modal: true
    closePolicy: Popup.NoAutoClose
    width: Math.min(parent ? parent.width - 80 : 600, 600)
    title: qsTr("Before you start")

    component Point: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.text
        font.pixelSize: Theme.fontBody
        textFormat: Text.StyledText
    }

    ColumnLayout {
        width: parent.width
        spacing: 12
        Point { text: qsTr("<b>This is unaudited development software.</b> It was largely written by a language model under human direction. It is tested; it has not been reviewed by a security professional. Do not rely on it for anything consequential yet.") }
        Point { text: qsTr("<b>Use a dedicated email address.</b> e3mail downloads your mail and, by default, removes it from the server, so the mailbox lives on this device. Mail already on the server when you connect is left alone.") }
        Point { text: qsTr("<b>It still works with everyone.</b> People who have never heard of e3mail receive ordinary email, and clients that support Autocrypt or OpenPGP get encrypted mail.") }
        Point { text: qsTr("<b>Back up your data folder.</b> There is no other copy of your mail.") }
    }
    footer: DialogButtonBox {
        Button {
            text: qsTr("I understand")
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }
    onAccepted: MailApp.acknowledgeFirstRun()
}
