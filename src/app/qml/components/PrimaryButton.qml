// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

Button {
    id: btn
    implicitHeight: 36
    contentItem: Text {
        text: btn.text
        color: Theme.accentText
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: Theme.radius
        color: btn.enabled ? (btn.down ? Qt.darker(Theme.accent, 1.15) : Theme.accent) : Qt.alpha(Theme.accent, 0.4)
    }
}
