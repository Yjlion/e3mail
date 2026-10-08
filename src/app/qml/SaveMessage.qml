// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

// Saving a message as a .eml file: as it was received or sent, or
// decrypted. Decrypted asks first, because the file is then readable by
// anyone who can open it.
Item {
    id: saver
    property int messageId: 0
    property bool decrypted: false

    function save(id, asDecrypted) {
        messageId = id
        decrypted = asDecrypted
        if (asDecrypted) warning.open()
        else pick()
    }
    function pick() {
        fileDialog.selectedFile = "file:///" + MailApp.suggestedEmlName(messageId)
        fileDialog.open()
    }

    Dialog {
        id: warning
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 440)
        modal: true
        title: qsTr("Save decrypted")
        standardButtons: Dialog.Save | Dialog.Cancel
        onAccepted: saver.pick()
        Label {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            text: qsTr("The saved file is not encrypted. Anyone who can open it can read the message.")
        }
    }

    FileDialog {
        id: fileDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "eml"
        nameFilters: [qsTr("Email messages (*.eml)"), qsTr("All files (*)")]
        onAccepted: MailApp.saveMessage(saver.messageId, selectedFile, saver.decrypted)
    }
}
