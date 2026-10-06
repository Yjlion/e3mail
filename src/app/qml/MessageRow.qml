// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: row
    // Model roles
    required property int messageId
    required property string from
    required property string to
    required property string subject
    required property string preview
    required property string date
    required property bool unread
    required property bool encrypted
    required property bool verified
    required property int importance
    required property bool hasAttachments
    required property var labels
    required property bool outgoing
    required property int state

    property bool selected: false
    signal activated()

    implicitHeight: content.implicitHeight + 20
    color: selected ? Theme.selection : mouse.containsMouse ? Theme.hover : Theme.surface

    Rectangle {
        visible: row.unread
        width: 3; height: parent.height - 12
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.accent
        radius: 1.5
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 14
        anchors.rightMargin: 12
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: row.state === 10 ? qsTr("Draft") + (row.to.length ? " — " + row.to : "")
                    : row.outgoing ? qsTr("To: ") + row.to : row.from
                color: row.state === 10 ? Theme.bad : Theme.text
                font.pixelSize: Theme.fontSmall + 1
                font.weight: row.unread ? Font.Bold : Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                text: row.state === 11 ? qsTr("Sending…") : row.state === 13 ? qsTr("Not sent") : row.date
                color: row.state === 13 ? Theme.bad : Theme.muted
                font.pixelSize: Theme.fontSmall
            }
        }
        Text {
            Layout.fillWidth: true
            text: row.subject
            color: Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: row.unread ? Font.Bold : Font.Normal
            elide: Text.ElideRight
        }
        Text {
            Layout.fillWidth: true
            text: row.preview
            color: Theme.muted
            font.pixelSize: Theme.fontSmall + 1
            elide: Text.ElideRight
            visible: text.length > 0
        }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 3
            spacing: 4
            Badge { visible: row.importance > 0; text: qsTr("important"); fg: Theme.warn; bg: Theme.warnBg }
            Badge { visible: row.encrypted; text: "e2e"; fg: Theme.good; bg: Theme.goodBg }
            Badge { visible: !row.encrypted && row.state !== 10; text: qsTr("unencrypted") }
            Badge { visible: row.verified; text: qsTr("verified"); fg: Theme.info; bg: Theme.infoBg }
            Badge { visible: row.hasAttachments; text: qsTr("attachment") }
            Repeater {
                model: row.labels
                Badge {
                    required property var modelData
                    text: modelData.name
                    fg: modelData.color.length ? modelData.color : Theme.muted
                }
            }
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: 1
        color: Theme.border
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (ev) => {
            row.activated()
            if (ev.button === Qt.RightButton) contextMenu.popup()
        }
    }
    MessageMenu {
        id: contextMenu
        messageId: row.messageId
        unread: row.unread
    }
}
