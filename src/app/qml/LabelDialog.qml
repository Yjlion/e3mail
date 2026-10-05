// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Dialog {
    id: dlg
    property int labelId: 0
    property string chosenColor: palette[0]
    readonly property var palette: ["#2457d6", "#1a7f4b", "#9a5b00", "#b42318", "#7c3aed", "#0e7490", "#be185d", "#636b78"]
    signal created(int id)

    function openFor(id, name, color) {
        labelId = id
        nameField.text = name
        chosenColor = color.length ? color : palette[0]
        open()
        nameField.forceActiveFocus()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    title: labelId ? qsTr("Edit tag") : qsTr("New tag")
    standardButtons: Dialog.Ok | Dialog.Cancel

    ColumnLayout {
        spacing: 10
        Field { id: nameField; Layout.preferredWidth: 280; placeholderText: qsTr("Name") }
        Row {
            spacing: 6
            Repeater {
                model: dlg.palette
                Rectangle {
                    required property string modelData
                    width: 22; height: 22; radius: 11
                    color: modelData
                    border.width: dlg.chosenColor === modelData ? 3 : 0
                    border.color: Theme.text
                    MouseArea { anchors.fill: parent; onClicked: dlg.chosenColor = parent.modelData }
                }
            }
        }
        Button {
            visible: dlg.labelId !== 0
            text: qsTr("Delete tag")
            flat: true
            onClicked: { MailApp.deleteLabel(dlg.labelId); dlg.reject() }
        }
    }
    onAccepted: {
        if (labelId) MailApp.renameLabel(labelId, nameField.text, chosenColor)
        else if (nameField.text.trim().length) created(MailApp.createLabel(nameField.text, chosenColor))
    }
}
