// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

Rectangle {
    id: view
    color: Theme.window
    property var contacts: []
    property var current: ({})
    // The phone rows being edited: [{label, number}]
    property var phones: []
    readonly property var phoneLabels: [{ v: "mobile", t: qsTr("Mobile") }, { v: "work", t: qsTr("Work") },
                                        { v: "home", t: qsTr("Home") }, { v: "other", t: qsTr("Other") }]

    function reload() { contacts = MailApp.contacts(search.text) }
    function show(id) {
        current = id ? MailApp.contact(id) : ({})
        phones = current.phones || []
    }
    // Everything the detail pane edits, in one go.
    function save() {
        if (!current.id) return
        MailApp.setContactDetails(current.id, {
            name: nameField.text, organization: orgField.text, title: titleField.text,
            birthday: birthdayField.text, notes: notesField.text, phones: phones
        })
        show(current.id)
        reload()
    }
    function setPhone(i, label, number) {
        const p = phones.slice()
        p[i] = { label: label, number: number }
        phones = p
    }
    onVisibleChanged: if (visible) reload()
    // Narrow: from a contact back to the list. False when already there.
    function back() {
        if (!Theme.narrow || !current.id) return false
        show(0)
        return true
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Narrow: the list, or the contact chosen from it.
        ColumnLayout {
            visible: !Theme.narrow || !view.current.id
            Layout.preferredWidth: Theme.listWidth
            Layout.maximumWidth: Theme.narrow ? Number.POSITIVE_INFINITY : Theme.listWidth
            Layout.fillWidth: Theme.narrow
            Layout.fillHeight: true
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 4
                Field {
                    id: search
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search contacts")
                    onTextChanged: view.reload()
                }
                IconButton { iconName: "plus"; tip: qsTr("New contact"); onClicked: newDialog.openNew() }
                ToolButton {
                    text: "⋯"
                    implicitWidth: 32
                    Accessible.name: qsTr("Import or export")
                    onClicked: bookMenu.popup()
                    Menu {
                        id: bookMenu
                        MenuItem { text: qsTr("Import vCard…"); onTriggered: importDialog.open() }
                        MenuItem { text: qsTr("Export contacts…"); onTriggered: exportDialog.open() }
                    }
                }
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
                    MouseArea { anchors.fill: parent; onClicked: view.show(modelData.id) }
                }
            }
        }
        Rectangle { visible: !Theme.narrow; Layout.fillHeight: true; width: 1; color: Theme.border }

        Item {
            visible: !Theme.narrow || !!view.current.id
            Layout.fillWidth: true
            Layout.fillHeight: true
            Text {
                anchors.centerIn: parent
                visible: !view.current.id
                text: qsTr("Select a contact")
                color: Theme.muted
            }
            Flickable {
                anchors.fill: parent
                visible: !!view.current.id
                contentHeight: detail.implicitHeight + 60
                clip: true
                ScrollBar.vertical: ScrollBar {}

                ColumnLayout {
                    id: detail
                    // Mirrored by hand: x is not.
                    x: LayoutMirroring.enabled ? parent.width - width - 32 : 32
                    y: 28
                    width: Math.min(parent.width - 64, 560)
                    spacing: 8
                    Field {
                        id: nameField
                        Layout.fillWidth: true
                        text: view.current.name || ""
                        placeholderText: qsTr("Name")
                        font.pixelSize: 18
                        onEditingFinished: if (text !== (view.current.name || "")) view.save()
                    }
                    Text { text: view.current.addr || ""; color: Theme.muted; font.pixelSize: Theme.fontBody }

                    GridLayout {
                        Layout.topMargin: 8
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 6
                        Label { text: qsTr("Organization"); color: Theme.muted }
                        Field {
                            id: orgField
                            Layout.fillWidth: true
                            text: view.current.organization || ""
                            onEditingFinished: if (text !== (view.current.organization || "")) view.save()
                        }
                        Label { text: qsTr("Title"); color: Theme.muted }
                        Field {
                            id: titleField
                            Layout.fillWidth: true
                            text: view.current.title || ""
                            onEditingFinished: if (text !== (view.current.title || "")) view.save()
                        }
                        Label { text: qsTr("Birthday"); color: Theme.muted }
                        Field {
                            id: birthdayField
                            Layout.fillWidth: true
                            text: view.current.birthday || ""
                            placeholderText: qsTr("YYYY-MM-DD, or --MM-DD without the year")
                            inputMethodHints: Qt.ImhDate
                            onEditingFinished: if (text !== (view.current.birthday || "")) view.save()
                        }
                        Label { text: qsTr("Phone"); color: Theme.muted; Layout.alignment: Qt.AlignTop; Layout.topMargin: 8 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Repeater {
                                model: view.phones
                                RowLayout {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    ChoiceBox {
                                        Layout.preferredWidth: 120
                                        model: view.phoneLabels
                                        textRole: "t"; valueRole: "v"
                                        selected: Math.max(0, ["mobile", "work", "home", "other"].indexOf(modelData.label))
                                        onActivated: { view.setPhone(index, currentValue, modelData.number); view.save() }
                                    }
                                    Field {
                                        Layout.fillWidth: true
                                        text: modelData.number
                                        inputMethodHints: Qt.ImhDialableCharactersOnly
                                        onEditingFinished: if (text !== modelData.number) { view.setPhone(index, modelData.label, text); view.save() }
                                    }
                                    IconButton {
                                        iconName: "close"
                                        tip: qsTr("Remove this number")
                                        onClicked: { const p = view.phones.slice(); p.splice(index, 1); view.phones = p; view.save() }
                                    }
                                }
                            }
                            Button {
                                flat: true
                                text: qsTr("Add a phone number")
                                onClicked: view.phones = view.phones.concat([{ label: "mobile", number: "" }])
                            }
                        }
                        Label { text: qsTr("Notes"); color: Theme.muted; Layout.alignment: Qt.AlignTop; Layout.topMargin: 8 }
                        TextArea {
                            id: notesField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 80
                            text: view.current.notes || ""
                            color: Theme.text
                            wrapMode: TextEdit.Wrap
                            onEditingFinished: if (text !== (view.current.notes || "")) view.save()
                            background: Rectangle { radius: Theme.radius; color: Theme.surface; border.color: Theme.border }
                        }
                    }

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
                    ChoiceBox {
                        Layout.preferredWidth: 320
                        model: [qsTr("Use the account setting"), qsTr("Lenient"), qsTr("Opportunistic"), qsTr("Strict — never unencrypted")]
                        selected: (view.current.encryption !== undefined ? view.current.encryption : -1) + 1
                        onActivated: (i) => MailApp.setContactEncryption(view.current.id, i - 1)
                    }
                    RowLayout {
                        Layout.topMargin: 12
                        Button {
                            visible: !view.current.known
                            text: qsTr("Accept")
                            onClicked: { MailApp.accept(view.current.addr); view.show(view.current.id); view.reload() }
                        }
                        Button {
                            text: view.current.blocked ? qsTr("Unblock") : qsTr("Block")
                            onClicked: {
                                if (view.current.blocked) MailApp.unblock(view.current.addr); else MailApp.block(view.current.addr)
                                view.show(view.current.id); view.reload()
                            }
                        }
                        Button {
                            text: qsTr("Remove contact")
                            flat: true
                            onClicked: removeConfirm.open()
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: newDialog
        function openNew() {
            newAddr.text = search.text.indexOf("@") >= 0 ? search.text.trim() : ""
            newName.text = ""
            open()
            newAddr.forceActiveFocus()
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("New contact")
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            spacing: 8
            Field {
                id: newAddr
                Layout.preferredWidth: 300
                placeholderText: qsTr("Email address")
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
            }
            Field { id: newName; Layout.preferredWidth: 300; placeholderText: qsTr("Name (optional)") }
            Label {
                Layout.preferredWidth: 300
                text: qsTr("Mail from your contacts goes straight to the Inbox.")
                color: Theme.muted
                wrapMode: Text.Wrap
            }
        }
        onAccepted: {
            const id = MailApp.createContact(newAddr.text, newName.text)
            if (id) { view.reload(); view.show(id) }
        }
    }

    Dialog {
        id: removeConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Remove %1?").arg(view.current.name || view.current.addr || "")
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: Math.min(360, Window.width - 96)
            text: qsTr("Their details are removed from your contacts. Their next mail waits in Unverified until you accept it. Their key is kept.")
            wrapMode: Text.Wrap
        }
        onAccepted: { MailApp.removeContact(view.current.id); view.show(0); view.reload() }
    }

    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("vCard files (*.vcf *.vcard)"), qsTr("All files (*)")]
        onAccepted: { MailApp.importContacts(selectedFile); view.reload() }
    }
    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "vcf"
        nameFilters: [qsTr("vCard files (*.vcf *.vcard)")]
        onAccepted: MailApp.exportContacts(selectedFile)
    }
}
