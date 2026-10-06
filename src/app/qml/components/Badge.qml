// SPDX-License-Identifier: MPL-2.0
import QtQuick
import E3mail

// A small rounded label: e2e, verified, important, attachment, a tag.
Rectangle {
    id: badge
    property string text
    property color fg: Theme.muted
    property color bg: "transparent"
    property bool outlined: bg === "transparent"

    implicitWidth: label.implicitWidth + 12
    implicitHeight: label.implicitHeight + 4
    radius: height / 2
    color: bg
    border.width: outlined ? 1 : 0
    border.color: Qt.alpha(fg, 0.45)

    Text {
        id: label
        anchors.centerIn: parent
        text: badge.text
        color: badge.fg
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
    }
}
