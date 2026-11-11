// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// The calendar (ADR 0025): a month with the day's or the coming days'
// events beside it (below it on a phone), or a week or a day as a grid of
// hours. Everything shown comes from this device; invitations arrive and go
// by mail.
Rectangle {
    id: view
    color: Theme.window
    // month | week | day. On a phone "week" is three days: seven do not fit.
    property string mode: MailApp.calendarMode
    // The day the view is about, as an ISO date: its month, its week, itself.
    property string anchor: iso(new Date())
    readonly property int year: day(anchor).getFullYear()
    readonly property int month: day(anchor).getMonth() // 0-based, as MonthGrid counts
    property string selectedDay: ""            // ISO date, or empty for the coming days
    property var events: []                    // occurrences of the shown month, plus the agenda's horizon
    property var byDay: ({})                   // ISO day -> [occurrence]
    property var grid: ({})                    // MailApp.calendarGrid, for the week and the day
    // Where Back returns to on a phone, after a day was opened from the week.
    property string returnMode: ""
    // How many days the grid shows, and whether the agenda is beside it
    // (the week is its own agenda).
    readonly property int span: mode === "day" ? 1 : Theme.narrow ? 3 : 7
    readonly property bool agendaShown: Theme.narrow ? mode === "month" : mode !== "week"

    function iso(d) { return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0") }
    function day(isoDate) { const p = isoDate.split("-"); return new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2])) }
    function plusDays(isoDate, n) { const d = day(isoDate); d.setDate(d.getDate() + n); return iso(d) }
    function reload() {
        // From `mode` and `anchor` themselves: this runs as they change,
        // when what is bound to them may not have followed yet.
        const days = mode === "day" ? 1 : Theme.narrow ? 3 : 7
        if (mode !== "month")
            grid = MailApp.calendarGrid(days === 7 ? MailApp.weekStart(anchor) : anchor, days)
        if (mode !== "month" && (Theme.narrow || mode === "week"))
            return
        const a = day(anchor)
        const from = new Date(a.getFullYear(), a.getMonth(), 1)
        from.setDate(from.getDate() - 7)
        const to = new Date(a.getFullYear(), a.getMonth() + 1, 1)
        to.setDate(to.getDate() + 7)
        const horizon = new Date()
        horizon.setDate(horizon.getDate() + 15)
        events = MailApp.calendarEvents(from, to > horizon ? to : horizon)
        const map = {}
        for (const e of events) {
            // A multi-day event sits on each of its days. They come as
            // dates: an all-day event's are its own, whatever this clock is.
            for (let k = e.day, n = 0; k <= e.lastDay && n < 62; k = plusDays(k, 1), ++n) {
                if (!map[k]) map[k] = []
                map[k].push(e)
            }
        }
        byDay = map
    }
    // The switch: remembered on this device.
    function choose(m) {
        returnMode = ""
        // Leaving the month with a day chosen: that day, or its week.
        if (mode === "month" && m !== "month" && selectedDay)
            anchor = selectedDay
        mode = m
        MailApp.calendarMode = m
    }
    // One day, from the week's header: Back returns to the week.
    function openDay(isoDate) { if (mode !== "day") returnMode = mode; anchor = isoDate; mode = "day" }
    function step(n) {
        if (mode === "month") {
            const d = new Date(year, month + n, 1)
            anchor = iso(d)
        } else {
            anchor = plusDays(anchor, n * span)
        }
    }
    function today() { const d = iso(new Date()); anchor = d; if (mode === "month") selectedDay = d }
    function showEvent(uid) { if (uid) eventDialog.edit(uid) }
    // For screenshots (--page calendar-event): the first event of the month, editing.
    function showFirst() { reload(); if (events.length) eventDialog.edit(events[0].uid) }
    function showNew() { eventDialog.create(mode === "month" ? selectedDay || iso(new Date()) : anchor) }
    // For screenshots (--page calendar-week, calendar-day): the busiest of
    // the coming weeks, and its busiest day. Not remembered.
    function showBusiest(m) {
        const from = new Date()
        const to = new Date()
        to.setDate(to.getDate() + 28)
        const count = {}, week = {}
        for (const e of MailApp.calendarEvents(from, to)) {
            count[e.day] = (count[e.day] || 0) + 1
            const w = MailApp.weekStart(e.day)
            week[w] = (week[w] || 0) + 1
        }
        const best = (map, among) => Object.keys(map).filter(among).sort().reduce((a, k) => !a || map[k] > map[a] ? k : a, "")
        const w = best(week, k => true)
        if (!w)
            return
        const d = best(count, k => MailApp.weekStart(k) === w)
        anchor = d
        mode = m
    }
    onVisibleChanged: if (visible) reload()
    onAnchorChanged: reload()
    onModeChanged: reload()
    onSpanChanged: reload()
    Connections {
        target: MailApp
        function onCalendarChanged() { if (view.visible) view.reload() }
        function onAccountChanged() { view.reload() }
    }
    // Narrow: from a day's events back to what is coming, or from a day
    // opened in the week back to the week. False when already there.
    function back() {
        if (!Theme.narrow) return false
        if (mode === "month" && selectedDay) { selectedDay = ""; return true }
        if (returnMode) { mode = returnMode; returnMode = ""; return true }
        return false
    }
    // Midnight passes: today is another day.
    property string todayIs: iso(new Date())
    Timer {
        interval: 60000
        running: view.visible
        repeat: true
        onTriggered: { const d = view.iso(new Date()); if (d !== view.todayIs) { view.todayIs = d; view.reload() } }
    }

    readonly property var agenda: {
        if (selectedDay && mode === "month") return byDay[selectedDay] || []
        const now = new Date()
        const start = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        return events.filter(e => new Date(e.end) >= start).slice(0, 60)
    }

    // A dragged event (TimeGrid): a meeting with attendees asks first.
    function moved(ev, apply) {
        if (ev.sent) eventDialog.askMove(ev, apply)
        else apply(false)
    }

    GridLayout {
        anchors.fill: parent
        columns: Theme.narrow ? 1 : 3
        rows: Theme.narrow ? 2 : 1
        columnSpacing: 0
        rowSpacing: 0

        // The month, or the hours. On a phone the month takes its own height
        // and the day's or the coming events sit below it.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: !Theme.narrow || view.mode !== "month"
            Layout.alignment: Qt.AlignTop
            spacing: 0
            RowLayout {
                id: header
                Layout.fillWidth: true
                Layout.margins: 10
                Layout.bottomMargin: Theme.narrow ? 4 : 10
                spacing: 4
                IconButton {
                    iconName: "chevron-left"
                    tip: view.mode === "month" ? qsTr("Previous month") : qsTr("Earlier")
                    onClicked: view.step(-1)
                }
                IconButton {
                    iconName: "chevron-right"
                    tip: view.mode === "month" ? qsTr("Next month") : qsTr("Later")
                    onClicked: view.step(1)
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    text: view.mode === "month" ? Qt.locale().standaloneMonthName(view.month, Locale.LongFormat) + " " + view.year
                                                : (Theme.narrow ? view.grid.briefTitle : view.grid.title) || ""
                    color: Theme.text
                    font.pixelSize: Theme.narrow && view.mode !== "month" ? Theme.fontBody + 2 : Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignLeft
                }
                Button { text: qsTr("Today"); flat: true; onClicked: view.today() }
                Segmented {
                    id: wideSwitch
                    visible: !Theme.narrow
                    model: [qsTr("Month"), qsTr("Week"), qsTr("Day")]
                    current: ["month", "week", "day"].indexOf(view.mode)
                    onPicked: (i) => view.choose(["month", "week", "day"][i])
                }
                IconButton { iconName: "plus"; tip: qsTr("New event"); onClicked: view.showNew() }
            }
            Segmented {
                visible: Theme.narrow
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                Layout.bottomMargin: 6
                model: [qsTr("Month"), qsTr("3 days"), qsTr("Day")]
                current: wideSwitch.current
                onPicked: (i) => view.choose(["month", "week", "day"][i])
            }

            // The month is made only while it is shown. Left in the layout
            // hidden from the start, its DayOfWeekRow and MonthGrid size
            // themselves from their own contents without end, and the window
            // never finishes laying out.
            Loader {
                active: view.mode === "month"
                visible: active
                Layout.fillWidth: true
                Layout.fillHeight: !Theme.narrow
                sourceComponent: Component {
                    ColumnLayout {
                        spacing: 0
                        DayOfWeekRow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            locale: Qt.locale()
                            delegate: Text {
                                required property var model
                                text: model.shortName
                                color: Theme.muted
                                font.pixelSize: Theme.fontSmall
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                        MonthGrid {
                            id: monthGrid
                            Layout.fillWidth: true
                            Layout.fillHeight: !Theme.narrow
                            Layout.leftMargin: 10
                            Layout.rightMargin: 10
                            Layout.bottomMargin: 10
                            implicitHeight: Theme.narrow ? 6 * 46 : 6 * 90
                            month: view.month
                            year: view.year
                            locale: Qt.locale()
                            delegate: Rectangle {
                                id: cell
                                required property var model
                                readonly property string key: view.iso(model.date)
                                readonly property var dayEvents: view.byDay[key] || []
                                readonly property bool selected: view.selectedDay === key
                                radius: Theme.radius
                                color: selected ? Theme.selection : model.today ? Theme.raised : "transparent"
                                border.color: model.today ? Theme.accent : "transparent"
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 3
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: cell.model.day
                                        horizontalAlignment: Text.AlignRight
                                        color: cell.model.month === monthGrid.month ? Theme.text : Theme.muted
                                        opacity: cell.model.month === monthGrid.month ? 1 : 0.5
                                        font.pixelSize: Theme.fontSmall + 1
                                        font.weight: cell.model.today ? Font.Bold : Font.Normal
                                    }
                                    Repeater {
                                        model: Theme.narrow ? 0 : Math.min(3, cell.dayEvents.length)
                                        Rectangle {
                                            required property int index
                                            readonly property var ev: cell.dayEvents[index]
                                            Layout.fillWidth: true
                                            implicitHeight: 16
                                            radius: 3
                                            color: ev.myStatus === "needs-action" ? "transparent" : ev.mine ? Theme.accent : Theme.info
                                            border.color: ev.mine ? Theme.accent : Theme.info
                                            border.width: ev.myStatus === "needs-action" ? 1 : 0
                                            opacity: ev.myStatus === "tentative" ? 0.6 : 1
                                            Text {
                                                anchors.fill: parent
                                                anchors.leftMargin: 4
                                                anchors.rightMargin: 4
                                                text: ev.title
                                                textFormat: Text.PlainText
                                                color: ev.myStatus === "needs-action" ? Theme.text : Theme.accentText
                                                font.pixelSize: Theme.fontSmall
                                                elide: Text.ElideRight
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                        }
                                    }
                                    Text {
                                        visible: Theme.narrow ? cell.dayEvents.length > 0 : cell.dayEvents.length > 3
                                        text: Theme.narrow ? "•".repeat(Math.min(3, cell.dayEvents.length)) : qsTr("+%n more", "", cell.dayEvents.length - 3)
                                        color: Theme.narrow ? Theme.accent : Theme.muted
                                        font.pixelSize: Theme.fontSmall
                                        Layout.alignment: Theme.narrow ? Qt.AlignHCenter : Qt.AlignLeft
                                    }
                                    Item { Layout.fillHeight: true }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: view.selectedDay = cell.key
                                    onDoubleClicked: { view.selectedDay = cell.key; view.showNew() }
                                }
                            }
                        }
                    }
                }
            }

            TimeGrid {
                id: timeGrid
                visible: view.mode !== "month"
                Layout.fillWidth: true
                Layout.fillHeight: true
                grid: view.grid
                onEventClicked: (uid) => eventDialog.edit(uid)
                onSlotClicked: (day, minutes) => eventDialog.create(day, minutes)
                onDayClicked: (day) => view.openDay(day)
                onEventMoved: (ev, day, startMin) => view.moved(ev, (send) => { MailApp.rescheduleEvent(ev.uid, day, startMin, send); view.reload() })
                onEventStretched: (ev, day, endMin) => view.moved(ev, (send) => { MailApp.stretchEvent(ev.uid, day, endMin, send); view.reload() })
            }
        }
        Rectangle { visible: !Theme.narrow && view.agendaShown; Layout.fillHeight: true; implicitWidth: 1; color: Theme.border }

        // The agenda: a day, or what is coming.
        ColumnLayout {
            visible: view.agendaShown
            Layout.fillHeight: true
            Layout.fillWidth: Theme.narrow
            Layout.preferredWidth: Theme.narrow ? -1 : 320
            Layout.maximumWidth: Theme.narrow ? Number.POSITIVE_INFINITY : 360
            spacing: 0
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 10
                Text {
                    Layout.fillWidth: true
                    text: view.selectedDay && view.mode === "month" ? MailApp.formatDay(view.selectedDay) : qsTr("Coming up")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody + 1
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                IconButton {
                    visible: !!view.selectedDay && view.mode === "month"
                    iconName: "close"
                    tip: qsTr("Show what is coming")
                    onClicked: view.selectedDay = ""
                }
            }
            ListView {
                id: agendaList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: view.agenda
                ScrollBar.vertical: ScrollBar {}
                section.property: "day"
                section.delegate: Text {
                    width: ListView.view.width
                    leftPadding: 12
                    topPadding: 8
                    bottomPadding: 2
                    text: MailApp.formatDay(section)
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                }
                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: row.implicitHeight + 14
                    color: hover.hovered ? Theme.hover : "transparent"
                    HoverHandler { id: hover }
                    RowLayout {
                        id: row
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10
                        Rectangle {
                            width: 4; height: 36; radius: 2
                            color: modelData.mine ? Theme.accent : Theme.info
                            opacity: modelData.myStatus === "needs-action" ? 0.35 : modelData.myStatus === "tentative" ? 0.6 : 1
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: modelData.title
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.timeText
                                color: Theme.muted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: !!modelData.location
                                text: modelData.location
                                textFormat: Text.PlainText
                                color: Theme.muted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                            }
                        }
                        Badge {
                            visible: modelData.myStatus === "needs-action" || modelData.myStatus === "tentative"
                            text: modelData.myStatus === "tentative" ? qsTr("Tentative") : qsTr("Not answered")
                            fg: Theme.warn
                            bg: Theme.warnBg
                        }
                        Badge { visible: modelData.recurring; text: qsTr("Repeats"); fg: Theme.muted }
                    }
                    MouseArea { anchors.fill: parent; onClicked: eventDialog.edit(modelData.uid) }
                }
                Text {
                    anchors.centerIn: parent
                    visible: agendaList.count === 0
                    width: parent.width - 40
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: view.selectedDay && view.mode === "month" ? qsTr("Nothing on this day.")
                                                                    : qsTr("Nothing coming up. Invitations you accept and events you add appear here.")
                    color: Theme.muted
                }
            }
            Button {
                Layout.margins: 10
                Layout.fillWidth: true
                text: qsTr("New event")
                onClicked: view.showNew()
            }
        }
    }

    EventDialog {
        id: eventDialog
        onSaved: view.reload()
    }
}
