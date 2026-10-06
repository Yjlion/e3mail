// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

// Toggle user tags on a message.
Menu {
    id: menu
    property int messageId
    readonly property var current: {
        for (let i = 0; i < MailApp.thread.length; ++i)
            if (MailApp.thread[i].id === messageId) return MailApp.thread[i].labels
        return []
    }
    function has(id) {
        for (let i = 0; i < current.length; ++i) if (current[i].id === id) return true
        return false
    }
    Repeater {
        model: MailApp.labels
        MenuItem {
            required property var modelData
            text: modelData.name
            checkable: true
            checked: menu.has(modelData.id)
            onTriggered: MailApp.setLabel(menu.messageId, modelData.id, checked)
        }
    }
    MenuItem {
        text: MailApp.labels.length ? qsTr("New tag…") : qsTr("Create a tag…")
        onTriggered: newTag.openFor(0, "", "")
    }
    LabelDialog {
        id: newTag
        onCreated: (id) => MailApp.setLabel(menu.messageId, id, true)
    }
}
