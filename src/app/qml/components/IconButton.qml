// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import E3mail

// A flat button showing one monochrome icon, with a tooltip.
ToolButton {
    id: btn
    property string iconName
    property string tip
    property bool on: false
    property color tint: on ? Theme.accent : Theme.muted
    // Arrows that point along the reading direction turn around in a
    // right-to-left layout.
    readonly property bool directional: ["reply", "reply-all", "forward", "restore"].indexOf(iconName) >= 0
    transform: Scale {
        origin.x: btn.width / 2
        xScale: btn.directional && btn.mirrored ? -1 : 1
    }

    icon.source: Theme.icon(iconName)
    icon.color: tint
    icon.width: 18
    icon.height: 18
    implicitWidth: 32
    implicitHeight: 32
    ToolTip.visible: hovered && tip.length > 0
    ToolTip.delay: 500
    ToolTip.text: tip
    Accessible.name: tip
    background: Rectangle {
        radius: Theme.radius
        color: btn.on ? Theme.selection : btn.hovered ? Theme.hover : "transparent"
    }
}
