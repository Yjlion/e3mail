// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A few choices side by side, one of them chosen: Month | Week | Day.
Rectangle {
    id: seg
    property var model: []      // the labels
    property int current: 0
    signal picked(int index)

    // Every choice as wide as the widest, so none is cut short.
    property real widest: 0
    function measure() {
        let w = 0
        for (let i = 0; i < choices.count; ++i) {
            const item = choices.itemAt(i)
            if (item) w = Math.max(w, item.implicitWidth)
        }
        widest = w
    }
    implicitWidth: widest * Math.max(1, model.length) + 4
    implicitHeight: 30
    radius: Theme.radius
    color: Theme.raised

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 2
        spacing: 0
        uniformCellSizes: true
        Repeater {
            id: choices
            model: seg.model
            onItemAdded: seg.measure()
            AbstractButton {
                id: choice
                required property string modelData
                required property int index
                readonly property bool chosen: index === seg.current
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitWidth: implicitContentWidth + leftPadding + rightPadding
                onImplicitWidthChanged: seg.measure()
                implicitHeight: 26
                leftPadding: 10
                rightPadding: 10
                text: modelData
                checkable: false
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checked: chosen
                onClicked: seg.picked(index)
                contentItem: Text {
                    text: choice.text
                    color: choice.chosen ? Theme.text : Theme.muted
                    font.pixelSize: Theme.fontBody
                    font.weight: choice.chosen ? Font.DemiBold : Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
                background: Rectangle {
                    radius: Theme.radius - 1
                    color: choice.chosen ? Theme.surface : choice.hovered ? Theme.hover : "transparent"
                    border.color: choice.chosen ? Theme.border : "transparent"
                }
            }
        }
    }
}
