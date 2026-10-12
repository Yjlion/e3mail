// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A rich-text editor with its formatting toolbar: the composer's body and the
// signature editor. A format chosen with nothing selected applies to what is
// typed next (TextFormatter).
ColumnLayout {
    id: editor
    spacing: 8

    property bool toolbar: true
    property alias area: body
    property alias textDocument: body.textDocument
    property alias formatter: formatter
    property alias placeholderText: body.placeholderText

    function fmt(kind) { formatter.toggle(kind); body.forceActiveFocus() }
    function block(kind) { formatter.block(kind); body.forceActiveFocus() }

    TextFormatter {
        id: formatter
        document: body.textDocument
        cursorPosition: body.cursorPosition
        selectionStart: body.selectionStart
        selectionEnd: body.selectionEnd
    }

    Shortcut { sequences: [StandardKey.Bold]; enabled: body.activeFocus; onActivated: editor.fmt("bold") }
    Shortcut { sequences: [StandardKey.Italic]; enabled: body.activeFocus; onActivated: editor.fmt("italic") }
    Shortcut { sequences: [StandardKey.Underline]; enabled: body.activeFocus; onActivated: editor.fmt("underline") }

    Flow {
        Layout.fillWidth: true
        visible: editor.toolbar
        spacing: 2
        // The buttons must not take focus, or the editor's cursor is lost.
        IconButton { focusPolicy: Qt.NoFocus; iconName: "bold"; tip: qsTr("Bold"); on: formatter.bold; onClicked: editor.fmt("bold") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "italic"; tip: qsTr("Italic"); on: formatter.italic; onClicked: editor.fmt("italic") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "underline"; tip: qsTr("Underline"); on: formatter.underline; onClicked: editor.fmt("underline") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "strike"; tip: qsTr("Strikethrough"); on: formatter.strike; onClicked: editor.fmt("strike") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "heading"; tip: qsTr("Heading"); on: formatter.heading; onClicked: editor.block("h2") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "quote"; tip: qsTr("Quote"); on: formatter.quote; onClicked: editor.block("quote") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "list-bullet"; tip: qsTr("Bulleted list"); on: formatter.bullet; onClicked: editor.block("bullet") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "list-number"; tip: qsTr("Numbered list"); on: formatter.numbered; onClicked: editor.block("numbered") }
        IconButton { focusPolicy: Qt.NoFocus; iconName: "code"; tip: qsTr("Code"); on: formatter.code; onClicked: editor.fmt("code") }
        IconButton {
            focusPolicy: Qt.NoFocus
            iconName: "link"; tip: qsTr("Link")
            enabled: body.selectedText.length > 0
            onClicked: linkPrompt.open()
        }
    }

    ScrollView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        TextArea {
            id: body
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            persistentSelection: true
            color: Theme.text
            font.pixelSize: Theme.fontBody + 1
            background: Rectangle {
                radius: Theme.radius
                color: Theme.surface
                border.color: body.activeFocus ? Theme.accent : Theme.border
            }
            padding: 12
        }
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
        onAccepted: { formatter.link(linkField.text); body.forceActiveFocus() }
    }
}
