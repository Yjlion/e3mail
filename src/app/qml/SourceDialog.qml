// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

Dialog {
    id: dlg
    function show(id) {
        area.text = MailApp.viewSource(id)
        open()
    }
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent.width - 80, 960)
    height: parent.height - 80
    modal: true
    title: qsTr("Message source")
    standardButtons: Dialog.Close
    ScrollView {
        anchors.fill: parent
        TextArea {
            id: area
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            textFormat: TextEdit.PlainText
            font.family: "monospace"
            font.pixelSize: 12
            color: Theme.text
        }
    }
}
