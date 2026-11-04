// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// The calendar (ADR 0025): a month, and the day's or the coming days'
// events beside it (below it on a phone). Everything shown comes from this
// device; invitations arrive and go by mail.
Rectangle {
    id: view
    color: Theme.window
    property int year: new Date().getFullYear()
    property int month: new Date().getMonth() // 0-based, as MonthGrid counts
    property string selectedDay: ""            // ISO date, or empty for the coming days
    property var events: []                    // occurrences of the shown month, plus the agenda's horizon
    property var byDay: ({})                   // ISO day -> [occurrence]

    function iso(d) { return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0") }
    function reload() {
        const from = new Date(year, month, 1)
        from.setDate(from.getDate() - 7)
        const to = new Date(year, month + 1, 1)
        to.setDate(to.getDate() + 7)
        const horizon = new Date()
        horizon.setDate(horizon.getDate() + 15)
        events = MailApp.calendarEvents(from, to > horizon ? to : horizon)
        const map = {}
        for (const e of events) {
            // A multi-day event sits on each of its days.
            const s = new Date(e.start), t = new Date(e.end)
            for (let d = new Date(s.getFullYear(), s.getMonth(), s.getDate()); d < t || map[iso(d)] === undefined && d <= s; d.setDate(d.getDate() + 1)) {
                const k = iso(d)
                if (!map[k]) map[k] = []
                map[k].push(e)
                if (e.allDay && d >= new Date(t.getTime() - 1)) break
            }
        }
        byDay = map
    }
    function today() { const d = new Date(); year = d.getFullYear(); month = d.getMonth(); selectedDay = iso(d) }
    function showEvent(uid) { if (uid) eventDialog.edit(uid) }
    // For screenshots (--page calendar-event): the first event of the month, editing.
    function showFirst() { reload(); if (events.length) eventDialog.edit(events[0].uid) }
    function showNew() { eventDialog.create(selectedDay || iso(new Date())) }
    onVisibleChanged: if (visible) reload()
    onYearChanged: reload()
    onMonthChanged: reload()
    Connections { target: MailApp; function onCalendarChanged() { if (view.visible) view.reload() } function onAccountChanged() { view.reload() } }
    // Narrow: from a day's events back to what is coming. False when already there.
    function back() {
        if (!Theme.narrow || !selectedDay) return false
        selectedDay = ""
        return true
    }

    readonly property var agenda: {
        if (selectedDay) return byDay[selectedDay] || []
        const now = new Date()
        const start = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        return events.filter(e => new Date(e.end) >= start).slice(0, 60)
    }

    GridLayout {
        anchors.fill: parent
        columns: Theme.narrow ? 1 : 3
        rows: Theme.narrow ? 2 : 1
        columnSpacing: 0
        rowSpacing: 0

        // The month. On a phone it takes its own height and the day's or the
        // coming events sit below it.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: !Theme.narrow
            Layout.alignment: Qt.AlignTop
            spacing: 0
            RowLayout {
                id: header
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 4
                IconButton { iconName: "back"; tip: qsTr("Previous month"); onClicked: { if (view.month === 0) { view.month = 11; view.year-- } else view.month-- } }
                Text {
                    Layout.fillWidth: true
                    text: Qt.locale().standaloneMonthName(view.month, Locale.LongFormat) + " " + view.year
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                IconButton { iconName: "forward"; tip: qsTr("Next month"); onClicked: { if (view.month === 11) { view.month = 0; view.year++ } else view.month++ } }
                Button { text: qsTr("Today"); flat: true; onClicked: view.today() }
                IconButton { iconName: "plus"; tip: qsTr("New event"); onClicked: view.showNew() }
            }
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
                id: grid
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
                            color: cell.model.month === grid.month ? Theme.text : Theme.muted
                            opacity: cell.model.month === grid.month ? 1 : 0.5
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
        Rectangle { visible: !Theme.narrow; Layout.fillHeight: true; width: 1; color: Theme.border }

        // The agenda: a day, or what is coming.
        ColumnLayout {
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
                    text: view.selectedDay ? MailApp.formatDay(view.selectedDay) : qsTr("Coming up")
                    color: Theme.text
                    font.pixelSize: Theme.fontBody + 1
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                IconButton { visible: !!view.selectedDay; iconName: "close"; tip: qsTr("Show what is coming"); onClicked: view.selectedDay = "" }
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
                    text: view.selectedDay ? qsTr("Nothing on this day.") : qsTr("Nothing coming up. Invitations you accept and events you add appear here.")
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
