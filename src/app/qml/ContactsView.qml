// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: view
    color: Theme.window
    property var contacts: []
    property var current: ({})

    function reload() { contacts = MailApp.contacts(search.text) }
    onVisibleChanged: if (visible) reload()

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.preferredWidth: Theme.listWidth
            Layout.maximumWidth: Theme.listWidth
            Layout.fillWidth: false
            Layout.fillHeight: true
            spacing: 0
            Field {
                id: search
                Layout.fillWidth: true
                Layout.margins: 10
                placeholderText: qsTr("Search contacts")
                onTextChanged: view.reload()
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: view.contacts
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: 54
                    color: view.current.id === modelData.id ? Theme.selection : hover.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: hover }
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            text: modelData.name.length ? modelData.name : modelData.addr
                            color: Theme.text
                            font.pixelSize: Theme.fontBody
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            spacing: 4
                            Text { text: modelData.addr; color: Theme.muted; font.pixelSize: Theme.fontSmall; Layout.fillWidth: true; elide: Text.ElideRight }
                            Badge { visible: modelData.verified; text: qsTr("verified"); fg: Theme.info; bg: Theme.infoBg }
                            Badge { visible: modelData.hasKey && !modelData.verified; text: qsTr("key"); fg: Theme.good }
                            Badge { visible: modelData.blocked; text: qsTr("blocked"); fg: Theme.bad }
                        }
                    }
                    MouseArea { anchors.fill: parent; onClicked: view.current = MailApp.contact(modelData.id) }
                }
            }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: Theme.border }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Text {
                anchors.centerIn: parent
                visible: !view.current.id
                text: qsTr("Select a contact")
                color: Theme.muted
            }
            ColumnLayout {
                visible: !!view.current.id
                x: 32; y: 28
                width: Math.min(parent.width - 64, 560)
                spacing: 8
                Field {
                    Layout.fillWidth: true
                    text: view.current.name || ""
                    placeholderText: qsTr("Name")
                    font.pixelSize: 18
                    onEditingFinished: { MailApp.setContactName(view.current.id, text); view.reload() }
                }
                Text { text: view.current.addr || ""; color: Theme.muted; font.pixelSize: Theme.fontBody }
                Text {
                    Layout.topMargin: 12
                    text: view.current.verified ? qsTr("Verified in person.")
                        : view.current.fingerprint ? qsTr("Key learned from their mail. Not verified: this proves continuity, not identity.")
                        : qsTr("No key yet. Mail to them goes unencrypted until they send you one.")
                    color: Theme.text
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Text {
                    visible: !!view.current.fingerprint
                    text: view.current.fingerprint || ""
                    font.family: "monospace"
                    color: Theme.muted
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                }
                Label { Layout.topMargin: 12; text: qsTr("Encryption with this contact"); color: Theme.muted }
                ComboBox {
                    Layout.preferredWidth: 320
                    model: [qsTr("Use the account setting"), qsTr("Lenient"), qsTr("Opportunistic"), qsTr("Strict — never unencrypted")]
                    currentIndex: (view.current.encryption !== undefined ? view.current.encryption : -1) + 1
                    onActivated: (i) => MailApp.setContactEncryption(view.current.id, i - 1)
                }
                RowLayout {
                    Layout.topMargin: 12
                    Button {
                        visible: !view.current.known
                        text: qsTr("Accept")
                        onClicked: { MailApp.accept(view.current.addr); view.current = MailApp.contact(view.current.id); view.reload() }
                    }
                    Button {
                        text: view.current.blocked ? qsTr("Unblock") : qsTr("Block")
                        onClicked: {
                            if (view.current.blocked) MailApp.unblock(view.current.addr); else MailApp.block(view.current.addr)
                            view.current = MailApp.contact(view.current.id); view.reload()
                        }
                    }
                }
            }
        }
    }
}
