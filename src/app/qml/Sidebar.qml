// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: sidebar
    property string page
    signal navigate(string page)
    signal composeRequested()

    color: Theme.sidebar

    function showTag(tag) { MailApp.selectTag(tag); navigate("mail") }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 24
        clip: true

        ColumnLayout {
            id: column
            x: 12
            y: 14
            width: sidebar.width - 24
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 4
                spacing: 6
                Image {
                    source: Theme.icon("e3mail")
                    sourceSize: Qt.size(22, 22)
                }
                Text {
                    text: "e3mail"
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.Bold
                }
                Badge {
                    text: "PREVIEW"
                    fg: Theme.warn
                    bg: Theme.warnBg
                    ToolTip.visible: previewHover.hovered
                    ToolTip.text: qsTr("Unaudited development software. Keep a backup of your data folder.")
                    HoverHandler { id: previewHover }
                }
                Item { Layout.fillWidth: true }
            }

            // The account picker appears only when there is a choice to make.
            ComboBox {
                id: accountPicker
                Layout.fillWidth: true
                Layout.topMargin: 10
                visible: MailApp.accounts.length > 1
                model: MailApp.accounts
                textRole: "addr"
                valueRole: "id"
                currentIndex: {
                    for (let i = 0; i < MailApp.accounts.length; ++i)
                        if (MailApp.accounts[i].id === MailApp.currentAccountId) return i
                    return 0
                }
                onActivated: (index) => MailApp.selectAccount(MailApp.accounts[index].id)
            }

            PrimaryButton {
                Layout.fillWidth: true
                Layout.topMargin: 10
                text: qsTr("Compose")
                onClicked: sidebar.composeRequested()
            }

            SectionLabel { text: qsTr("Mailbox") }
            NavItem {
                text: qsTr("Inbox"); count: MailApp.counts.inbox || 0
                active: page === "mail" && MailApp.currentTag === "inbox"
                onClicked: showTag("inbox")
            }
            NavItem {
                text: qsTr("Unverified"); count: MailApp.counts.unverified || 0; warnCount: true
                active: page === "mail" && MailApp.currentTag === "unverified"
                onClicked: showTag("unverified")
            }
            NavItem {
                text: qsTr("Sent"); count: MailApp.counts.outbox || 0
                active: page === "mail" && MailApp.currentTag === "sent"
                onClicked: showTag("sent")
            }
            NavItem {
                text: qsTr("Drafts"); count: MailApp.counts.drafts || 0
                active: page === "mail" && MailApp.currentTag === "drafts"
                onClicked: showTag("drafts")
            }
            NavItem {
                text: qsTr("Archive")
                active: page === "mail" && MailApp.currentTag === "archive"
                onClicked: showTag("archive")
            }
            NavItem {
                text: qsTr("Trash")
                active: page === "mail" && MailApp.currentTag === "trash"
                onClicked: showTag("trash")
            }

            SectionLabel { text: qsTr("Tags"); visible: true }
            Repeater {
                model: MailApp.labels
                NavItem {
                    required property var modelData
                    text: modelData.name
                    dot: modelData.color.length ? modelData.color : Theme.muted
                    active: page === "mail" && MailApp.currentLabelId === modelData.id
                    onClicked: { MailApp.selectLabel(modelData.id); sidebar.navigate("mail") }
                }
            }
            Text {
                Layout.leftMargin: 10
                Layout.topMargin: 2
                text: qsTr("New tag…")
                color: Theme.muted
                font.pixelSize: Theme.fontSmall
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: labelDialog.openFor(0, "", "")
                }
            }

            SectionLabel { text: qsTr("Account") }
            NavItem {
                text: qsTr("Contacts"); active: page === "contacts"
                onClicked: sidebar.navigate("contacts")
            }
            NavItem {
                text: qsTr("Settings"); active: page === "settings"
                onClicked: sidebar.navigate("settings")
            }
            Text {
                Layout.leftMargin: 10
                Layout.topMargin: 2
                text: qsTr("Add a mailbox…")
                color: Theme.muted
                font.pixelSize: Theme.fontSmall
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.navigate("setup")
                }
            }

            // Connection state, stated plainly.
            RowLayout {
                Layout.topMargin: 18
                Layout.leftMargin: 10
                spacing: 6
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: MailApp.accountStatus === "online" ? Theme.good
                         : MailApp.accountStatus === "auth" ? Theme.bad : Theme.warn
                }
                Text {
                    Layout.fillWidth: true
                    text: MailApp.accountStatus === "online" ? qsTr("Connected")
                        : MailApp.accountStatus === "auth" ? qsTr("Login failed — check the password")
                        : qsTr("Offline")
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                    elide: Text.ElideRight
                    ToolTip.visible: statusHover.hovered && MailApp.accountStatusDetail.length > 0
                    ToolTip.text: MailApp.accountStatusDetail
                    HoverHandler { id: statusHover }
                }
            }
        }
    }

    LabelDialog { id: labelDialog }
}
