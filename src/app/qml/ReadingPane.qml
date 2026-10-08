// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: pane
    color: Theme.window
    signal reply(int id, bool all)
    signal forward(int id)
    signal editDraft(int id)

    readonly property var thread: MailApp.thread
    readonly property var last: thread.length ? thread[thread.length - 1] : null
    function showSource(id) { sourceDialog.show(id) }

    Text {
        anchors.centerIn: parent
        visible: !pane.last
        text: qsTr("Select a message")
        color: Theme.muted
        font.pixelSize: Theme.fontBody
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: pane.last !== null

        // Toolbar for the conversation.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 2
            // A subject aligns by its own script, so an English subject in a
            // right-to-left interface would sit at the far edge. As wide as
            // its text, it goes where the row puts it: the start edge.
            Text {
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth + 1
                Layout.leftMargin: 10
                text: pane.last ? pane.last.subject : ""
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true }
            IconButton {
                iconName: "reply"; tip: qsTr("Reply")
                visible: pane.last && pane.last.state !== 10
                onClicked: pane.reply(pane.last.id, false)
            }
            IconButton {
                iconName: "reply-all"; tip: qsTr("Reply all")
                visible: pane.last && pane.last.state !== 10
                onClicked: pane.reply(pane.last.id, true)
            }
            IconButton {
                iconName: "forward"; tip: qsTr("Forward")
                visible: pane.last && pane.last.state !== 10
                onClicked: pane.forward(pane.last.id)
            }
            IconButton {
                iconName: "archive"; tip: pane.last && pane.last.archived ? qsTr("Move to Inbox") : qsTr("Archive")
                visible: pane.last && !pane.last.trashed
                onClicked: MailApp.archive(MailApp.selectedMessageId, !pane.last.archived)
            }
            IconButton {
                iconName: "tag"; tip: qsTr("Tags")
                onClicked: tagMenu.popup()
                TagMenu { id: tagMenu; messageId: MailApp.selectedMessageId }
            }
            IconButton {
                iconName: pane.last && pane.last.trashed ? "restore" : "trash"
                tip: pane.last && pane.last.trashed ? qsTr("Restore") : qsTr("Move to Trash")
                onClicked: pane.last.trashed ? MailApp.restore(MailApp.selectedMessageId)
                                             : MailApp.trash(MailApp.selectedMessageId)
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        ListView {
            id: cards
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 12
            topMargin: Theme.narrow ? 10 : 16
            bottomMargin: 24
            model: pane.thread
            ScrollBar.vertical: ScrollBar {}
            boundsBehavior: Flickable.StopAtBounds
            // Open on the newest message.
            onCountChanged: Qt.callLater(() => positionViewAtEnd())
            // The list places its delegates at x = 0, so the margin is inside
            // (a leftMargin that changes with the width is not re-applied).
            delegate: Item {
                id: slot
                required property var modelData
                required property int index
                width: cards.width
                height: card.height
                MessageCard {
                    id: card
                    x: Theme.narrow ? 10 : 20
                    width: Math.min(cards.width - 2 * x, 900)
                    modelData: slot.modelData
                    index: slot.index
                    // Earlier messages in a thread start folded.
                    expanded: index === cards.count - 1 || modelData.id === MailApp.selectedMessageId
                    onEditDraft: (id) => pane.editDraft(id)
                    onShowSource: (id) => sourceDialog.show(id)
                    onAskOpenLink: (url) => linkDialog.ask(url)
                }
            }
        }
    }

    SourceDialog { id: sourceDialog }
    LinkDialog { id: linkDialog }
}
