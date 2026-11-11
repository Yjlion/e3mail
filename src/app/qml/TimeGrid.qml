// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A week, three days or one day as a grid of hours (ADR 0025, amendment
// 2026-11-10). It draws what MailApp.calendarGrid laid out and says what
// the person did; it changes nothing itself.
//
// Days run along the reading direction, so everything here is placed by
// dayX() rather than by a Row: a bar spans several days.
Item {
    id: tg
    property var grid: ({})
    readonly property var days: grid.days || []
    readonly property var bars: grid.bars || []
    readonly property var hours: grid.hours || []
    readonly property int maxLanes: 4
    readonly property int lanes: Math.min(grid.lanes || 0, maxLanes)
    readonly property real hourHeight: 48
    readonly property real total: 24 * hourHeight
    // The shortest a block is drawn: CalendarLayout::kMinDrawn minutes.
    readonly property real minBlock: 20 / 60 * hourHeight
    // Dragging snaps to this many minutes.
    readonly property int step: 15
    readonly property bool rtl: LayoutMirroring.enabled
    readonly property real gutter: Math.ceil(hourMetrics.width) + 14
    readonly property real dayWidth: Math.max(1, (width - gutter) / Math.max(1, days.length))
    property int nowMin: 0
    // What is being dragged: {ev, day (index), startMin, endMin}.
    property var drag: null

    signal eventClicked(string uid)
    signal slotClicked(string day, int minutes)
    signal dayClicked(string day)
    // Dropped: the event starts at `startMin` on `day`, or ends at `endMin` on it.
    signal eventMoved(var ev, string day, int startMin)
    signal eventStretched(var ev, string day, int endMin)

    function dayX(i) { return rtl ? width - gutter - (i + 1) * dayWidth : gutter + i * dayWidth }
    function dayAt(x) {
        const i = Math.floor((rtl ? width - gutter - x : x - gutter) / dayWidth)
        return Math.max(0, Math.min(days.length - 1, i))
    }
    function snapped(minutes, low, high) { return Math.max(low, Math.min(high, Math.round(minutes / step) * step)) }
    function timeLabel(minutes) {
        return new Date(2000, 0, 1, Math.floor(minutes / 60) % 24, minutes % 60).toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
    }
    function minutesNow() { const d = new Date(); return d.getHours() * 60 + d.getMinutes() }

    // Opens at seven in the morning, or around now when today is shown;
    // once for each span of days, so that saving an event does not jump.
    property string placedFor: ""
    function place() {
        if (flick.height <= 0 || !grid.first)
            return
        const key = grid.first + "/" + days.length
        if (placedFor === key)
            return
        placedFor = key
        const today = days.some(d => d.today)
        const want = today ? minutesNow() / 60 * hourHeight - flick.height / 3 : 7 * hourHeight - 8
        flick.contentY = Math.max(0, Math.min(total - flick.height, want))
    }
    onGridChanged: { drag = null; Qt.callLater(place) }

    Timer {
        interval: 30000
        running: tg.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: tg.nowMin = tg.minutesNow()
    }
    TextMetrics {
        id: hourMetrics
        font.pixelSize: Theme.fontSmall
        text: tg.hours.reduce((a, b) => b.length > a.length ? b : a, "")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The days' names. One day alone is named by the page's title.
        Item {
            visible: tg.days.length > 1
            Layout.fillWidth: true
            implicitHeight: 48
            Repeater {
                model: tg.days
                Item {
                    id: head
                    required property var modelData
                    required property int index
                    x: tg.dayX(index)
                    width: tg.dayWidth
                    height: parent.height
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label
                    Accessible.onPressAction: tg.dayClicked(modelData.iso)
                    Column {
                        anchors.centerIn: parent
                        spacing: 1
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: head.modelData.weekday
                            color: head.modelData.today ? Theme.accent : Theme.muted
                            font.pixelSize: Theme.fontSmall
                        }
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: Math.max(26, number.implicitWidth + 10)
                            height: 26
                            radius: 13
                            color: head.modelData.today ? Theme.accent : headHover.hovered ? Theme.hover : "transparent"
                            Text {
                                id: number
                                anchors.centerIn: parent
                                text: head.modelData.number
                                color: head.modelData.today ? Theme.accentText : Theme.text
                                font.pixelSize: Theme.fontBody + 2
                                font.weight: head.modelData.today ? Font.Bold : Font.Normal
                            }
                        }
                    }
                    HoverHandler { id: headHover }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: tg.dayClicked(head.modelData.iso) }
                }
            }
        }

        // All-day events, and those a day long or longer.
        Item {
            visible: tg.bars.length > 0
            Layout.fillWidth: true
            implicitHeight: tg.lanes * 22 + 4
            clip: true
            Repeater {
                model: tg.bars
                Rectangle {
                    id: bar
                    required property var modelData
                    readonly property bool pending: modelData.myStatus === "needs-action"
                    visible: modelData.lane < tg.maxLanes
                    x: tg.dayX(tg.rtl ? modelData.firstDay + modelData.days - 1 : modelData.firstDay) + 2
                    y: modelData.lane * 22 + 2
                    width: modelData.days * tg.dayWidth - 4
                    height: 20
                    radius: 4
                    color: pending ? Theme.surface : modelData.mine ? Theme.accent : Theme.info
                    border.color: modelData.mine ? Theme.accent : Theme.info
                    border.width: pending ? 1 : 0
                    opacity: modelData.myStatus === "tentative" ? 0.6 : 1
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.title
                    Accessible.onPressAction: tg.eventClicked(modelData.uid)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 5
                        anchors.rightMargin: 5
                        spacing: 3
                        // It began before these days, or goes on after them. Right
                        // to left the text engine turns these two marks around itself.
                        Text { visible: bar.modelData.before; text: "‹"; color: label.color; font.pixelSize: Theme.fontSmall }
                        Text {
                            id: label
                            Layout.fillWidth: true
                            text: bar.modelData.title
                            textFormat: Text.PlainText
                            color: bar.pending ? Theme.text : Theme.accentText
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignLeft
                        }
                        Text { visible: bar.modelData.after; text: "›"; color: label.color; font.pixelSize: Theme.fontSmall }
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: tg.eventClicked(bar.modelData.uid) }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }

        Flickable {
            id: flick
            objectName: "hours"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: tg.total
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            onHeightChanged: tg.place()

            Item {
                id: canvas
                objectName: "canvas"
                width: flick.width
                height: tg.total

                // The hours: a line across the days, and its label beside them.
                Repeater {
                    model: 24
                    Rectangle {
                        required property int index
                        visible: index > 0
                        x: tg.rtl ? 0 : tg.gutter - 4
                        y: index * tg.hourHeight
                        width: canvas.width - tg.gutter + 4
                        height: 1
                        color: Theme.border
                    }
                }
                Repeater {
                    model: tg.hours
                    Text {
                        required property string modelData
                        required property int index
                        visible: index > 0
                        x: tg.rtl ? canvas.width - tg.gutter + 8 : 0
                        y: index * tg.hourHeight - height / 2
                        width: tg.gutter - 8
                        horizontalAlignment: Text.AlignRight
                        text: modelData
                        color: Theme.muted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Repeater {
                    model: tg.days
                    Item {
                        id: dayCol
                        required property var modelData
                        required property int index
                        x: tg.dayX(index)
                        width: tg.dayWidth
                        height: tg.total

                        Rectangle { visible: dayCol.modelData.today; anchors.fill: parent; color: Theme.accent; opacity: 0.05 }
                        Rectangle { x: tg.rtl ? parent.width - 1 : 0; width: 1; height: parent.height; color: Theme.border }
                        // An empty half hour: a new event there.
                        MouseArea {
                            anchors.fill: parent
                            onClicked: (m) => tg.slotClicked(dayCol.modelData.iso, Math.min(47, Math.floor(m.y / tg.hourHeight * 2)) * 30)
                        }

                        Repeater {
                            model: dayCol.modelData.blocks
                            Rectangle {
                                id: block
                                required property var modelData
                                readonly property var ev: modelData
                                readonly property bool pending: ev.myStatus === "needs-action"
                                readonly property real slot: (dayCol.width - 5) / ev.columns
                                readonly property real tall: Math.max((ev.endMin - ev.startMin) / 60 * tg.hourHeight, tg.minBlock)
                                readonly property bool lifted: !!tg.drag && tg.drag.ev.uid === ev.uid
                                x: 2 + (tg.rtl ? ev.columns - 1 - ev.column : ev.column) * slot + (tg.rtl ? 2 : 0)
                                y: Math.min(ev.startMin / 60 * tg.hourHeight, tg.total - tall)
                                width: slot - 1
                                height: tall - 1
                                radius: 4
                                color: pending ? Theme.surface : ev.mine ? Theme.accent : Theme.info
                                border.color: ev.mine ? Theme.accent : Theme.info
                                border.width: pending ? 1 : 0
                                opacity: (ev.myStatus === "tentative" ? 0.6 : 1) * (lifted ? 0.35 : 1)
                                clip: true
                                Accessible.role: Accessible.Button
                                Accessible.name: ev.title + ", " + ev.timeText
                                Accessible.onPressAction: tg.eventClicked(ev.uid)

                                Column {
                                    anchors.fill: parent
                                    anchors.leftMargin: 5
                                    anchors.rightMargin: 4
                                    anchors.topMargin: block.height < 24 ? 0 : 3
                                    Text {
                                        width: parent.width
                                        text: block.ev.title
                                        textFormat: Text.PlainText
                                        color: block.pending ? Theme.text : Theme.accentText
                                        font.pixelSize: Theme.fontSmall
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignLeft
                                    }
                                    Text {
                                        visible: block.height >= 30
                                        width: parent.width
                                        text: block.ev.timeText
                                        color: block.pending ? Theme.muted : Theme.accentText
                                        opacity: block.pending ? 1 : 0.85
                                        font.pixelSize: Theme.fontSmall
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignLeft
                                    }
                                    Text {
                                        visible: block.height >= 46 && !!block.ev.location
                                        width: parent.width
                                        text: block.ev.location
                                        textFormat: Text.PlainText
                                        color: block.pending ? Theme.muted : Theme.accentText
                                        opacity: block.pending ? 1 : 0.85
                                        font.pixelSize: Theme.fontSmall
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignLeft
                                    }
                                }

                                // A click opens it. With a mouse, an event that may be
                                // moved is dragged to another time or day; a finger
                                // scrolls the hours instead.
                                MouseArea {
                                    id: grab
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    property point from
                                    property real held: 0 // minutes between the event's start and the pointer
                                    property bool moving: false
                                    onPressed: (m) => {
                                        moving = false
                                        from = mapToItem(canvas, m.x, m.y)
                                        held = from.y / tg.hourHeight * 60 - block.ev.startMin
                                        preventStealing = block.ev.movable && !block.ev.before && m.source === Qt.MouseEventNotSynthesized
                                    }
                                    onPositionChanged: (m) => {
                                        if (!pressed || !preventStealing)
                                            return
                                        const p = mapToItem(canvas, m.x, m.y)
                                        if (!moving && Math.abs(p.x - from.x) + Math.abs(p.y - from.y) < 8)
                                            return
                                        moving = true
                                        const start = tg.snapped(p.y / tg.hourHeight * 60 - held, 0, 1440 - tg.step)
                                        const length = Math.max(tg.step, (block.ev.after ? 1440 : block.ev.endMin) - block.ev.startMin)
                                        tg.drag = { ev: block.ev, day: tg.dayAt(p.x), startMin: start, endMin: Math.min(1440, start + length) }
                                    }
                                    onReleased: {
                                        const d = tg.drag
                                        const was = moving
                                        moving = false
                                        tg.drag = null
                                        if (!was) {
                                            if (contains(Qt.point(mouseX, mouseY)))
                                                tg.eventClicked(block.ev.uid)
                                        } else if (d && (d.day !== dayCol.index || d.startMin !== block.ev.startMin))
                                            tg.eventMoved(block.ev, tg.days[d.day].iso, d.startMin)
                                    }
                                    onCanceled: { moving = false; tg.drag = null }
                                }
                                // The lower edge, dragged with a mouse: when it ends.
                                MouseArea {
                                    visible: block.ev.movable && !block.ev.after
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: Math.min(7, block.height / 3)
                                    cursorShape: Qt.SizeVerCursor
                                    onPressed: (m) => {
                                        preventStealing = m.source === Qt.MouseEventNotSynthesized
                                        m.accepted = preventStealing // a finger's tap goes to the event
                                    }
                                    onPositionChanged: (m) => {
                                        if (!pressed)
                                            return
                                        const p = mapToItem(canvas, m.x, m.y)
                                        const end = tg.snapped(p.y / tg.hourHeight * 60, block.ev.startMin + tg.step, 1440)
                                        tg.drag = { ev: block.ev, day: dayCol.index, startMin: block.ev.startMin, endMin: end }
                                    }
                                    onReleased: {
                                        const d = tg.drag
                                        tg.drag = null
                                        if (d && d.endMin !== block.ev.endMin)
                                            tg.eventStretched(block.ev, dayCol.modelData.iso, d.endMin)
                                    }
                                    onCanceled: tg.drag = null
                                }
                            }
                        }

                        // Now.
                        Rectangle {
                            visible: dayCol.modelData.today
                            y: tg.nowMin / 60 * tg.hourHeight - 1
                            width: parent.width
                            height: 2
                            color: Theme.bad
                            Rectangle { x: tg.rtl ? parent.width - 4 : -4; y: -3; width: 8; height: 8; radius: 4; color: Theme.bad }
                        }
                    }
                }

                // Where a dragged event would land.
                Rectangle {
                    visible: !!tg.drag
                    x: tg.drag ? tg.dayX(tg.drag.day) + 2 : 0
                    y: tg.drag ? tg.drag.startMin / 60 * tg.hourHeight : 0
                    width: tg.dayWidth - 5
                    height: tg.drag ? Math.max((tg.drag.endMin - tg.drag.startMin) / 60 * tg.hourHeight, tg.minBlock) - 1 : 0
                    radius: 4
                    color: Qt.alpha(Theme.accent, 0.25)
                    border.color: Theme.accent
                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: 5
                        anchors.rightMargin: 4
                        anchors.topMargin: 2
                        text: tg.drag ? tg.timeLabel(tg.drag.startMin) + " – " + tg.timeLabel(tg.drag.endMin) : ""
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                    }
                }
            }
        }
    }
}
