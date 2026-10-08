// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

// Right-click menu on a message row.
Menu {
    id: menu
    property int messageId
    property bool unread
    MenuItem { text: menu.unread ? qsTr("Mark as read") : qsTr("Mark as unread"); onTriggered: MailApp.markRead(menu.messageId, menu.unread) }
    MenuItem {
        text: MailApp.currentTag === "archive" ? qsTr("Move to Inbox") : qsTr("Archive")
        onTriggered: MailApp.archive(menu.messageId, MailApp.currentTag !== "archive")
    }
    MenuItem {
        text: MailApp.currentTag === "trash" ? qsTr("Restore") : qsTr("Move to Trash")
        onTriggered: MailApp.currentTag === "trash" ? MailApp.restore(menu.messageId) : MailApp.trash(menu.messageId)
    }
    MenuSeparator {}
    TagMenu { title: qsTr("Tags"); messageId: menu.messageId }
}
