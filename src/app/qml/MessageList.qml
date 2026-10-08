// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: root
    color: Theme.surface
    signal openDraft(int id)
    signal opened(int id) // a message was chosen, for the narrow layout

    function focusSearch() { search.forceActiveFocus(); search.selectAll() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6
            Field {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search mail")
                onTextChanged: searchDelay.restart()
                Timer { id: searchDelay; interval: 250; onTriggered: MailApp.setSearch(search.text) }
                Keys.onEscapePressed: text = ""
            }
            IconButton {
                iconName: "refresh"
                tip: qsTr("Check for new mail (F5)")
                onClicked: MailApp.syncNow()
            }
        }

        // A one-line explanation for the views that need one.
        Rectangle {
            Layout.fillWidth: true
            visible: hint.text.length > 0
            implicitHeight: hint.implicitHeight + 16
            color: Theme.raised
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                Text {
                    id: hint
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                    text: MailApp.currentTag === "unverified"
                          ? qsTr("Mail from senders you have not accepted waits here, then moves to Trash.")
                          : MailApp.currentTag === "trash"
                          ? qsTr("Messages here are destroyed when their time runs out.") : ""
                }
                Button {
                    visible: MailApp.currentTag === "trash" && list.count > 0
                    text: qsTr("Empty Trash")
                    flat: true
                    onClicked: emptyConfirm.open()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: MailApp.messages
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            keyNavigationEnabled: true
            focus: true
            currentIndex: MailApp.messages.indexOf(MailApp.selectedMessageId)
            Keys.onUpPressed: if (currentIndex > 0) MailApp.selectMessage(MailApp.messages.idAt(currentIndex - 1))
            Keys.onDownPressed: if (currentIndex < count - 1) MailApp.selectMessage(MailApp.messages.idAt(currentIndex + 1))
            Keys.onDeletePressed: if (MailApp.selectedMessageId) MailApp.trash(MailApp.selectedMessageId)
            // E archives, or in Archive moves back to the Inbox.
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_E && event.modifiers === Qt.NoModifier && MailApp.selectedMessageId
                        && MailApp.currentTag !== "trash") {
                    MailApp.archive(MailApp.selectedMessageId, MailApp.currentTag !== "archive")
                    event.accepted = true
                }
            }

            delegate: MessageRow {
                width: ListView.view.width
                selected: messageId === MailApp.selectedMessageId
                onActivated: {
                    list.forceActiveFocus()
                    if (state === 10) root.openDraft(messageId)
                    else { MailApp.selectMessage(messageId); root.opened(messageId) }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text: MailApp.searchText.length ? qsTr("Nothing matches.") : qsTr("No messages.")
                color: Theme.muted
                font.pixelSize: Theme.fontBody
            }
        }
    }

    Dialog {
        id: emptyConfirm
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Empty Trash?")
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: Math.min(implicitWidth, Window.width - 96)
            wrapMode: Text.Wrap
            text: qsTr("Every message in Trash is destroyed now. This cannot be undone.")
        }
        onAccepted: MailApp.emptyTrash()
    }
}
