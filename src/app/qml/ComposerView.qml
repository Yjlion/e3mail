// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

Rectangle {
    id: view
    color: Theme.window
    signal closed()

    property bool showCc: false
    property bool showBcc: false
    property bool formatting: false

    function start(kind, msgId) {
        if (kind === "reply") composer.startReply(msgId, false)
        else if (kind === "replyAll") composer.startReply(msgId, true)
        else if (kind === "forward") composer.startForward(msgId)
        else if (kind === "draft") composer.openDraft(msgId)
        else composer.startNew()
        showCc = composer.cc.length > 0
        showBcc = composer.bcc.length > 0
        composer.loadInto(body.textDocument)
        body.cursorPosition = 0
        if (composer.to.length) body.forceActiveFocus()
        else toField.forceActiveFocus()
    }

    // Close, keeping what was written as a draft.
    function close() { composer.saveDraft(body.textDocument); view.closed() }

    function fmt(kind) {
        composer.toggleFormat(body.textDocument, body.selectionStart, body.selectionEnd, kind)
        body.forceActiveFocus()
    }
    function block(kind) {
        composer.setBlock(body.textDocument, body.selectionStart, body.selectionEnd, kind)
        body.forceActiveFocus()
    }

    Composer {
        id: composer
        onSent: { MailApp.notify(qsTr("Sending…")); view.closed() }
        onFailed: (message) => { errorText.text = message; errorBox.visible = true }
    }

    Shortcut { sequence: "Ctrl+Return"; enabled: view.visible && composer.canSend; onActivated: composer.send(body.textDocument) }
    Shortcut { sequence: StandardKey.Bold; enabled: view.visible; onActivated: view.fmt("bold") }
    Shortcut { sequence: StandardKey.Italic; enabled: view.visible; onActivated: view.fmt("italic") }
    Shortcut { sequence: StandardKey.Underline; enabled: view.visible; onActivated: view.fmt("underline") }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.narrow ? 12 : 22
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Text {
                Layout.fillWidth: true
                text: composer.title
                color: Theme.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
            Button {
                text: qsTr("Close")
                flat: true
                onClicked: view.close()
            }
        }

        component HeaderRow: RowLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 4
            spacing: 8
        }
        component Line: Rectangle { Layout.fillWidth: true; Layout.maximumWidth: 900; height: 1; color: Theme.border }

        Line { Layout.topMargin: 12 }
        HeaderRow {
            Text { text: qsTr("To"); color: Theme.muted; font.pixelSize: Theme.fontBody; Layout.preferredWidth: 56 }
            AddressField { id: toField; Layout.fillWidth: true; text: composer.to; onTextChanged: composer.to = text }
            Text {
                visible: !view.showCc; text: qsTr("Cc"); color: Theme.muted; font.pixelSize: Theme.fontBody
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: view.showCc = true }
            }
            Text {
                visible: !view.showBcc; text: qsTr("Bcc"); color: Theme.muted; font.pixelSize: Theme.fontBody
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: view.showBcc = true }
            }
        }
        Line { visible: view.showCc }
        HeaderRow {
            visible: view.showCc
            Text { text: qsTr("Cc"); color: Theme.muted; font.pixelSize: Theme.fontBody; Layout.preferredWidth: 56 }
            AddressField { Layout.fillWidth: true; text: composer.cc; onTextChanged: composer.cc = text }
        }
        Line { visible: view.showBcc }
        HeaderRow {
            visible: view.showBcc
            Text { text: qsTr("Bcc"); color: Theme.muted; font.pixelSize: Theme.fontBody; Layout.preferredWidth: 56 }
            AddressField { Layout.fillWidth: true; text: composer.bcc; onTextChanged: composer.bcc = text }
        }
        Line {}
        HeaderRow {
            Text { text: qsTr("Subject"); color: Theme.muted; font.pixelSize: Theme.fontBody; Layout.preferredWidth: 56 }
            TextField {
                Layout.fillWidth: true
                text: composer.subject
                onTextChanged: composer.subject = text
                color: Theme.text
                font.pixelSize: Theme.fontBody
                background: Item {}
            }
        }
        Line {}

        // Formatting toolbar, shown when asked for.
        Flow {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 8
            visible: view.formatting
            spacing: 2
            IconButton { iconName: "bold"; tip: qsTr("Bold"); onClicked: view.fmt("bold") }
            IconButton { iconName: "italic"; tip: qsTr("Italic"); onClicked: view.fmt("italic") }
            IconButton { iconName: "underline"; tip: qsTr("Underline"); onClicked: view.fmt("underline") }
            IconButton { iconName: "strike"; tip: qsTr("Strikethrough"); onClicked: view.fmt("strike") }
            IconButton { iconName: "heading"; tip: qsTr("Heading"); onClicked: view.block("h2") }
            IconButton { iconName: "quote"; tip: qsTr("Quote"); onClicked: view.block("quote") }
            IconButton { iconName: "list-bullet"; tip: qsTr("Bulleted list"); onClicked: view.block("bullet") }
            IconButton { iconName: "list-number"; tip: qsTr("Numbered list"); onClicked: view.block("numbered") }
            IconButton { iconName: "code"; tip: qsTr("Code"); onClicked: view.fmt("code") }
            IconButton {
                iconName: "link"; tip: qsTr("Link")
                enabled: body.selectedText.length > 0
                onClicked: linkPrompt.open()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.maximumWidth: 900
            Layout.topMargin: 8
            TextArea {
                id: body
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                color: Theme.text
                font.pixelSize: Theme.fontBody + 1
                placeholderText: qsTr("Write your message")
                background: Rectangle {
                    radius: Theme.radius
                    color: Theme.surface
                    border.color: body.activeFocus ? Theme.accent : Theme.border
                }
                padding: 12
            }
        }

        // Attachments
        Flow {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 6
            spacing: 6
            visible: composer.attachments.length > 0
            Repeater {
                model: composer.attachments
                Rectangle {
                    required property var modelData
                    required property int index
                    radius: Theme.radius
                    color: Theme.raised
                    implicitWidth: chip.implicitWidth + 12
                    implicitHeight: 30
                    RowLayout {
                        id: chip
                        anchors.centerIn: parent
                        Text { text: modelData.name; color: Theme.text; font.pixelSize: Theme.fontSmall + 1 }
                        Text { text: modelData.size; color: Theme.muted; font.pixelSize: Theme.fontSmall }
                        IconButton {
                            iconName: "close"; tip: qsTr("Remove")
                            implicitWidth: 22; implicitHeight: 22
                            onClicked: composer.removeAttachment(index)
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 8
            text: composer.readiness
            color: composer.canSend ? (composer.encrypt ? Theme.good : Theme.muted) : Theme.bad
            font.pixelSize: Theme.fontSmall + 1
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 8
            spacing: 4
            PrimaryButton {
                text: qsTr("Send")
                Layout.preferredWidth: 96
                enabled: composer.canSend
                onClicked: composer.send(body.textDocument)
            }
            Item { width: 8 }
            IconButton { iconName: "heading"; tip: qsTr("Formatting"); on: view.formatting; onClicked: view.formatting = !view.formatting }
            IconButton { iconName: "attach"; tip: qsTr("Attach files"); onClicked: attachDialog.open() }
            IconButton { iconName: "signature"; tip: qsTr("Insert signature"); onClicked: composer.insertSignature(body.textDocument, body.cursorPosition) }
            IconButton { iconName: "flag"; tip: qsTr("Mark as important"); on: composer.important; onClicked: composer.important = !composer.important }
            IconButton {
                iconName: composer.encrypt ? "lock" : "lock-open"
                tip: composer.padlockLocked ? qsTr("Encryption is required for these recipients")
                     : composer.encrypt ? qsTr("Encrypted — click to send unencrypted") : qsTr("Not encrypted — click to require encryption")
                on: composer.encrypt
                tint: composer.encrypt ? Theme.good : Theme.muted
                enabled: !composer.padlockLocked
                onClicked: composer.encrypt = !composer.encrypt
            }
            Item { Layout.fillWidth: true }
            // On a phone, Close keeps the draft and the row has no room.
            Button { visible: !Theme.narrow; text: qsTr("Save draft"); flat: true; onClicked: { composer.saveDraft(body.textDocument); MailApp.notify(qsTr("Draft saved.")) } }
            IconButton { iconName: "trash"; tip: qsTr("Discard"); onClicked: { composer.discard(); view.closed() } }
        }

        Rectangle {
            id: errorBox
            Layout.fillWidth: true
            Layout.maximumWidth: 900
            Layout.topMargin: 8
            visible: false
            radius: Theme.radius
            color: Theme.badBg
            implicitHeight: errorText.implicitHeight + 16
            Text {
                id: errorText
                anchors.fill: parent
                anchors.margins: 8
                color: Theme.bad
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSmall + 1
            }
            MouseArea { anchors.fill: parent; onClicked: errorBox.visible = false }
        }
    }

    FileDialog {
        id: attachDialog
        fileMode: FileDialog.OpenFiles
        onAccepted: composer.attach(selectedFiles)
    }
    Dialog {
        id: linkPrompt
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Link to")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Field { id: linkField; width: Math.min(360, Window.width - 96); placeholderText: qsTr("example.com or name@example.com") }
        onOpened: { linkField.text = ""; linkField.forceActiveFocus() }
        onAccepted: composer.setLink(body.textDocument, body.selectionStart, body.selectionEnd, linkField.text)
    }
}
