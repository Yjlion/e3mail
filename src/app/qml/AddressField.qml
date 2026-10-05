// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

// A recipient line with suggestions from the address book.
TextField {
    id: field
    color: Theme.text
    font.pixelSize: Theme.fontBody
    selectByMouse: true
    background: Item {}
    property var suggestions: []

    function currentToken() {
        const parts = text.split(",")
        return parts[parts.length - 1].trim()
    }
    function complete(choice) {
        const parts = text.split(",")
        parts[parts.length - 1] = " " + choice
        text = parts.join(",").replace(/^\s+/, "") + ", "
        popup.close()
    }

    onTextEdited: {
        suggestions = MailApp.completeAddress(currentToken())
        if (suggestions.length) popup.open(); else popup.close()
    }
    Keys.onDownPressed: if (popup.opened) list.incrementCurrentIndex()
    Keys.onUpPressed: if (popup.opened) list.decrementCurrentIndex()
    Keys.onReturnPressed: (ev) => { if (popup.opened && list.currentIndex >= 0) complete(suggestions[list.currentIndex]); else ev.accepted = false }
    Keys.onEscapePressed: popup.close()

    Popup {
        id: popup
        y: field.height
        width: Math.min(field.width, 420)
        padding: 4
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
        contentItem: ListView {
            id: list
            implicitHeight: contentHeight
            model: field.suggestions
            currentIndex: 0
            delegate: ItemDelegate {
                required property string modelData
                required property int index
                width: ListView.view.width
                text: modelData
                highlighted: ListView.isCurrentItem
                onClicked: field.complete(modelData)
            }
        }
    }
}
