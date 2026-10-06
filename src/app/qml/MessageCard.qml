// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

Rectangle {
    id: card
    required property var modelData
    required property int index
    property bool expanded: true
    signal editDraft(int id)
    signal showSource(int id)
    signal askOpenLink(string url)

    readonly property var m: modelData

    implicitHeight: body.implicitHeight + 28
    radius: 8
    color: Theme.surface
    border.color: Theme.border

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        spacing: 8

        // Unverified: say so, and offer the two ways out.
        Rectangle {
            Layout.fillWidth: true
            visible: card.m.held
            radius: Theme.radius
            color: Theme.warnBg
            implicitHeight: heldRow.implicitHeight + 16
            RowLayout {
                id: heldRow
                anchors.fill: parent
                anchors.margins: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.warn
                    font.pixelSize: Theme.fontSmall + 1
                    text: card.m.heldDaysLeft >= 0
                          ? qsTr("You have not accepted this sender. Unless you do, this moves to Trash in %n day(s).", "", card.m.heldDaysLeft)
                          : qsTr("You have not accepted this sender.")
                }
                Button { text: qsTr("Accept"); onClicked: MailApp.accept(card.m.fromAddr) }
                Button { text: qsTr("Block"); flat: true; onClicked: MailApp.block(card.m.fromAddr) }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: card.m.trashed
            radius: Theme.radius
            color: Theme.badBg
            implicitHeight: trashText.implicitHeight + 16
            Text {
                id: trashText
                anchors.fill: parent
                anchors.margins: 8
                wrapMode: Text.Wrap
                color: Theme.bad
                font.pixelSize: Theme.fontSmall + 1
                text: card.m.trashReason + " " + qsTr("It will be destroyed in %n day(s) unless restored.", "", card.m.purgeDays)
            }
        }

        // Header
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Rectangle {
                width: 34; height: 34; radius: 17
                color: Theme.raised
                Text {
                    anchors.centerIn: parent
                    text: (card.m.from || "?").charAt(0).toUpperCase()
                    color: Theme.muted
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Text {
                    // At the start edge whatever its script: see ReadingPane.
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth + 1
                    text: card.m.from + (card.m.from !== card.m.fromAddr ? "  <" + card.m.fromAddr + ">" : "")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    visible: card.expanded
                    text: qsTr("To: ") + card.m.to
                          + (card.m.cc.length ? "   " + qsTr("Cc: ") + card.m.cc : "")
                          + (card.m.bcc.length ? "   " + qsTr("Bcc: ") + card.m.bcc : "")
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall + 1
                    wrapMode: Text.Wrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: !card.expanded
                    text: card.m.text.replace(/\s+/g, " ").substring(0, 140)
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall + 1
                    elide: Text.ElideRight
                }
            }
            Text {
                text: card.expanded ? card.m.date : card.m.shortDate
                color: Theme.muted
                font.pixelSize: Theme.fontSmall
            }
        }

        // What the encryption actually was. "Encrypted" and "verified" are
        // separate claims; only verification survives an active attacker.
        Flow {
            Layout.fillWidth: true
            visible: card.expanded
            spacing: 6
            Badge { visible: card.m.importance > 0; text: qsTr("important"); fg: Theme.warn; bg: Theme.warnBg }
            Badge {
                text: card.m.encrypted ? qsTr("end-to-end encrypted") : qsTr("not encrypted")
                fg: card.m.encrypted ? Theme.good : Theme.muted
                bg: card.m.encrypted ? Theme.goodBg : "transparent"
            }
            Badge {
                visible: !card.m.outgoing
                text: card.m.verified ? qsTr("verified sender")
                    : card.m.signed ? qsTr("signed, not verified") : qsTr("unsigned")
                fg: card.m.verified ? Theme.info : Theme.muted
                bg: card.m.verified ? Theme.infoBg : "transparent"
            }
            Repeater {
                model: card.m.labels
                Badge {
                    required property var modelData
                    text: modelData.name
                    fg: modelData.color.length ? modelData.color : Theme.muted
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: card.expanded && card.m.undelivered.length > 0
            radius: Theme.radius
            color: Theme.badBg
            implicitHeight: undeliveredText.implicitHeight + 12
            Text {
                id: undeliveredText
                anchors.fill: parent
                anchors.margins: 6
                wrapMode: Text.Wrap
                color: Theme.bad
                font.pixelSize: Theme.fontSmall + 1
                text: qsTr("Not delivered to: %1 (the server refused them).").arg(card.m.undelivered)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: card.expanded && card.m.remoteBlocked > 0
            text: qsTr("%n remote image(s) or style(s) not loaded.", "", card.m.remoteBlocked) + " " + qsTr("e3mail never loads remote content.")
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: card.expanded }

        // The body. HTML has already been through the sanitizer; plain text is
        // shown as plain text.
        TextEdit {
            Layout.fillWidth: true
            visible: card.expanded
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            textFormat: card.m.html.length ? TextEdit.RichText : TextEdit.PlainText
            text: card.m.html.length ? card.m.html : card.m.text
            color: Theme.text
            selectionColor: Theme.selection
            selectedTextColor: Theme.text
            font.pixelSize: Theme.fontBody + 1
            onLinkActivated: (link) => card.askOpenLink(link)
            HoverHandler { cursorShape: parent.hoveredLink.length ? Qt.PointingHandCursor : Qt.IBeamCursor }
        }

        // Attachments
        Flow {
            Layout.fillWidth: true
            visible: card.expanded && card.m.attachments.length > 0
            spacing: 8
            Repeater {
                model: card.m.attachments
                Rectangle {
                    required property var modelData
                    radius: Theme.radius
                    color: Theme.raised
                    implicitWidth: attRow.implicitWidth + 16
                    implicitHeight: 36
                    RowLayout {
                        id: attRow
                        anchors.centerIn: parent
                        spacing: 6
                        Image { source: Theme.icon("attach"); sourceSize: Qt.size(16, 16); opacity: 0.6 }
                        Text { text: modelData.name; color: Theme.text; font.pixelSize: Theme.fontSmall + 1 }
                        Text { text: modelData.size; color: Theme.muted; font.pixelSize: Theme.fontSmall }
                        IconButton {
                            iconName: "save"; tip: qsTr("Save…")
                            implicitWidth: 26; implicitHeight: 26
                            onClicked: {
                                saveDialog.attachmentIndex = modelData.index
                                saveDialog.selectedFile = "file:///" + MailApp.suggestedFileName(card.m.id, modelData.index)
                                saveDialog.open()
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: card.expanded
            spacing: 4
            Button {
                visible: card.m.state === 10
                text: qsTr("Edit draft")
                onClicked: card.editDraft(card.m.id)
            }
            Item { Layout.fillWidth: true }
            IconButton { iconName: "source"; tip: qsTr("View source"); onClicked: card.showSource(card.m.id) }
            IconButton {
                iconName: "mark-unread"; tip: qsTr("Mark unread")
                visible: !card.m.outgoing
                onClicked: MailApp.markRead(card.m.id, false)
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: !card.expanded
        cursorShape: Qt.PointingHandCursor
        onClicked: card.expanded = true
    }

    FileDialog {
        id: saveDialog
        property int attachmentIndex: -1
        fileMode: FileDialog.SaveFile
        onAccepted: MailApp.saveAttachment(card.m.id, attachmentIndex, selectedFile)
    }
}
