// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

TextField {
    id: field
    color: Theme.text
    placeholderTextColor: Theme.muted
    selectByMouse: true
    font.pixelSize: Theme.fontBody
    implicitHeight: 34
    background: Rectangle {
        radius: Theme.radius
        color: Theme.surface
        border.color: field.activeFocus ? Theme.accent : Theme.border
        border.width: 1
    }
}
