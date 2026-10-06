// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Following a link is a decision the user makes, not one a message makes:
// show the real target first.
Dialog {
    id: dlg
    property string url
    function ask(u) { url = u; open() }
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: qsTr("Open link?")
    standardButtons: Dialog.Open | Dialog.Cancel
    ColumnLayout {
        Label { text: qsTr("This link goes to:") }
        TextField {
            Layout.preferredWidth: 460
            readOnly: true
            text: dlg.url
            selectByMouse: true
        }
    }
    onAccepted: MailApp.openLink(url)
}
