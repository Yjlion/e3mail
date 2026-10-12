// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Writes one signature: a name and formatted text, through the composer's
// own editor, so it holds exactly what can be sent.
Dialog {
    id: dlg
    property string sigId

    function edit(id) {
        sigId = id
        nameField.text = id ? MailApp.signatureName(id) : ""
        MailApp.loadSignature(id, editor.textDocument)
        open()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(560, parent.width - (Theme.narrow ? 16 : 96))
    height: Math.min(460, parent.height - (Theme.narrow ? 16 : 96))
    title: sigId ? qsTr("Edit signature") : qsTr("New signature")
    standardButtons: Dialog.Save | Dialog.Cancel

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Field {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("Name, such as Work")
        }
        RichEditor {
            id: editor
            Layout.fillWidth: true
            Layout.fillHeight: true
            placeholderText: qsTr("Your signature")
        }
    }
    onOpened: editor.area.forceActiveFocus()
    onAccepted: MailApp.saveSignature(sigId, nameField.text, editor.textDocument)
}
