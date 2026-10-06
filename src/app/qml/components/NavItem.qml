// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Layouts
import E3mail

// One row in the sidebar.
Rectangle {
    id: item
    property string text
    property int count: 0
    property bool active: false
    property color dot: "transparent"
    property bool warnCount: false
    signal clicked()

    Layout.fillWidth: true
    implicitHeight: 32
    color: active ? Theme.selection : mouse.containsMouse ? Theme.hover : "transparent"
    radius: Theme.radius

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8
        Rectangle {
            visible: item.dot !== Qt.color("transparent")
            width: 8; height: 8; radius: 4
            color: item.dot
        }
        Text {
            Layout.fillWidth: true
            text: item.text
            color: item.active ? Theme.accent : Theme.text
            font.pixelSize: Theme.fontBody
            font.weight: item.active ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }
        Rectangle {
            visible: item.count > 0
            implicitWidth: Math.max(20, countText.implicitWidth + 10)
            implicitHeight: 18
            radius: 9
            color: item.warnCount ? Theme.warnBg : Theme.raised
            Text {
                id: countText
                anchors.centerIn: parent
                text: item.count
                font.pixelSize: Theme.fontSmall
                font.weight: Font.DemiBold
                color: item.warnCount ? Theme.warn : Theme.muted
            }
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: item.clicked()
    }
    Accessible.role: Accessible.Button
    Accessible.name: text
}
