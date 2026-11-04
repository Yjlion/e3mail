// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

// One event (ADR 0025): new, or opened from the calendar. Attendees are
// invited by mail; saving a change to a meeting we organize offers to send
// the update.
Dialog {
    id: dlg
    property string uid
    property var e: ({})
    // Everything the editor can hold, in one place.
    property var byDay: []
    readonly property bool mine: !uid || e.mine === true || e.mine === undefined
    readonly property bool hasAttendees: attendeesField.text.trim().length > 0
    readonly property var policy: hasAttendees ? MailApp.attendeePolicy(attendeesField.text) : ({ encrypt: false, missing: [] })
    signal saved(string uid)

    function create(dateIso) {
        uid = ""
        e = ({})
        titleField.text = ""
        allDayBox.checked = false
        const d = dateIso || new Date().toISOString().slice(0, 10)
        startDate.date = d
        endDate.date = d
        startTime.text = "09:00"
        endTime.text = "10:00"
        locationField.text = ""
        urlField.text = ""
        descriptionArea.text = ""
        attendeesField.text = ""
        freqBox.selected = 0
        intervalBox.value = 1
        byDay = []
        endsBox.selected = 0
        countBox.value = 10
        untilDate.date = d
        reminderBox.selected = reminderIndex(MailApp.calendarReminderDefault())
        open()
    }
    function edit(id) {
        uid = id
        e = MailApp.calendarEvent(id)
        titleField.text = e.title || ""
        allDayBox.checked = !!e.allDay
        startDate.date = e.startDate || ""
        endDate.date = e.endDate || ""
        startTime.text = e.startTime || "09:00"
        endTime.text = e.endTime || "10:00"
        locationField.text = e.location || ""
        urlField.text = e.url || ""
        descriptionArea.text = e.description || ""
        attendeesField.text = e.attendees || ""
        freqBox.selected = Math.max(0, ["none", "daily", "weekly", "monthly", "yearly", "other"].indexOf(e.freq || "none"))
        intervalBox.value = e.interval || 1
        byDay = e.byDay || []
        endsBox.selected = Math.max(0, ["never", "count", "until"].indexOf(e.ends || "never"))
        countBox.value = e.count || 10
        untilDate.date = e.untilDate || e.startDate || ""
        reminderBox.selected = reminderIndex(e.reminder === undefined ? -1 : e.reminder)
        open()
    }
    readonly property var reminderChoices: [-1, 0, 5, 10, 15, 30, 60, 120, 1440]
    function reminderIndex(minutes) {
        const i = reminderChoices.indexOf(minutes)
        return i < 0 ? 4 : i
    }
    function fields() {
        return {
            uid: uid, title: titleField.text, allDay: allDayBox.checked,
            startDate: startDate.date, startTime: startTime.text, endDate: endDate.date, endTime: endTime.text,
            location: locationField.text, url: urlField.text, description: descriptionArea.text,
            attendees: attendeesField.text,
            freq: ["none", "daily", "weekly", "monthly", "yearly", "other"][freqBox.currentIndex],
            interval: intervalBox.value, byDay: byDay,
            ends: ["never", "count", "until"][endsBox.currentIndex], count: countBox.value, untilDate: untilDate.date,
            reminder: reminderChoices[reminderBox.currentIndex]
        }
    }
    function save(send) {
        const id = MailApp.saveEvent(fields(), send)
        if (id) { dlg.saved(id); dlg.close() }
    }
    function toggleDay(d) {
        const list = byDay.slice()
        const i = list.indexOf(d)
        if (i < 0) list.push(d); else list.splice(i, 1)
        byDay = list
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(620, parent.width - (Theme.narrow ? 16 : 96))
    height: Math.min(640, parent.height - (Theme.narrow ? 16 : 96))
    title: uid ? qsTr("Edit event") : qsTr("New event")
    standardButtons: Dialog.NoButton

    contentItem: ColumnLayout {
        spacing: 6
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: form.implicitHeight
            clip: true
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: form
                width: parent.width
                spacing: 6
                Field {
                    id: titleField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Title")
                    font.pixelSize: 16
                    enabled: dlg.mine
                }
                CheckBox {
                    id: allDayBox
                    text: qsTr("All day")
                    enabled: dlg.mine
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: Theme.narrow ? 2 : 3
                    columnSpacing: 8
                    rowSpacing: 4
                    enabled: dlg.mine
                    Label { text: qsTr("Starts"); color: Theme.muted }
                    DateField { id: startDate; Layout.fillWidth: true; onDateChanged: if (endDate.date < date) endDate.date = date }
                    Field {
                        id: startTime
                        visible: !allDayBox.checked
                        Layout.preferredWidth: 80
                        Layout.columnSpan: Theme.narrow ? 2 : 1
                        placeholderText: "HH:MM"
                        inputMethodHints: Qt.ImhTime
                        validator: RegularExpressionValidator { regularExpression: /([01]\d|2[0-3]):[0-5]\d/ }
                    }
                    Item { visible: allDayBox.checked && !Theme.narrow; width: 1 }
                    Label { text: qsTr("Ends"); color: Theme.muted }
                    DateField { id: endDate; Layout.fillWidth: true }
                    Field {
                        id: endTime
                        visible: !allDayBox.checked
                        Layout.preferredWidth: 80
                        Layout.columnSpan: Theme.narrow ? 2 : 1
                        placeholderText: "HH:MM"
                        inputMethodHints: Qt.ImhTime
                        validator: RegularExpressionValidator { regularExpression: /([01]\d|2[0-3]):[0-5]\d/ }
                    }
                    Item { visible: allDayBox.checked && !Theme.narrow; width: 1 }
                }
                Text {
                    visible: !allDayBox.checked
                    text: qsTr("Times are in this device's time zone.")
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                }
                Label { text: qsTr("Repeats"); color: Theme.muted }
                RowLayout {
                    Layout.fillWidth: true
                    enabled: dlg.mine
                    ChoiceBox {
                        id: freqBox
                        Layout.preferredWidth: 170
                        model: [qsTr("Never"), qsTr("Every day"), qsTr("Every week"), qsTr("Every month"), qsTr("Every year"),
                                qsTr("As received")]
                    }
                    RowText { visible: freqBox.currentIndex >= 1 && freqBox.currentIndex <= 4; text: qsTr("every"); color: Theme.muted }
                    SpinBox {
                        id: intervalBox
                        visible: freqBox.currentIndex >= 1 && freqBox.currentIndex <= 4
                        from: 1; to: 99
                        editable: true
                    }
                    RowText {
                        visible: freqBox.currentIndex >= 1 && freqBox.currentIndex <= 4
                        color: Theme.muted
                        text: [ "", qsTr("day(s)"), qsTr("week(s)"), qsTr("month(s)"), qsTr("year(s)") ][freqBox.currentIndex] || ""
                    }
                }
                Text {
                    visible: freqBox.currentIndex === 5
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("This rule came with the invitation and is kept as it is: %1").arg(dlg.e.rrule || "")
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                }
                Flow {
                    visible: freqBox.currentIndex === 2
                    Layout.fillWidth: true
                    spacing: 4
                    enabled: dlg.mine
                    Repeater {
                        model: 7
                        Button {
                            required property int index
                            readonly property int day: ((Qt.locale().firstDayOfWeek + index - 1) % 7) + 1
                            text: Qt.locale().dayName(day, Locale.ShortFormat)
                            checkable: true
                            checked: dlg.byDay.indexOf(day) >= 0
                            onClicked: dlg.toggleDay(day)
                        }
                    }
                }
                RowLayout {
                    visible: freqBox.currentIndex >= 1 && freqBox.currentIndex <= 4
                    Layout.fillWidth: true
                    enabled: dlg.mine
                    RowText { text: qsTr("Ends"); color: Theme.muted }
                    ChoiceBox {
                        id: endsBox
                        Layout.preferredWidth: 150
                        model: [qsTr("Never"), qsTr("After"), qsTr("On")]
                    }
                    SpinBox { id: countBox; visible: endsBox.currentIndex === 1; from: 1; to: 999; editable: true }
                    RowText { visible: endsBox.currentIndex === 1; text: qsTr("time(s)"); color: Theme.muted }
                    DateField { id: untilDate; visible: endsBox.currentIndex === 2; Layout.fillWidth: true }
                }
                Label { text: qsTr("Where"); color: Theme.muted }
                Field { id: locationField; Layout.fillWidth: true; enabled: dlg.mine }
                Label { text: qsTr("Link"); color: Theme.muted }
                Field { id: urlField; Layout.fillWidth: true; placeholderText: "https://"; enabled: dlg.mine; inputMethodHints: Qt.ImhUrlCharactersOnly }
                Label { text: qsTr("Attendees"); color: Theme.muted }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 34
                    radius: Theme.radius
                    color: Theme.surface
                    border.color: attendeesField.activeFocus ? Theme.accent : Theme.border
                    AddressField {
                        id: attendeesField
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        placeholderText: qsTr("Names or addresses, separated by commas")
                        placeholderTextColor: Theme.muted
                        enabled: dlg.mine
                    }
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                    visible: dlg.mine
                    text: !dlg.hasAttendees ? qsTr("Attendees get an invitation by email, and an update when the meeting changes.")
                        : dlg.policy.encrypt ? qsTr("The invitation will be encrypted: every attendee has a key.")
                        : dlg.policy.missing.length ? qsTr("The invitation goes unencrypted: no key yet for %1.").arg(dlg.policy.missing.join(", "))
                        : qsTr("The invitation goes unencrypted.")
                }
                // Someone else's meeting: who is coming, read only.
                ColumnLayout {
                    visible: !dlg.mine && (dlg.e.attendeeList || []).length > 0
                    Layout.fillWidth: true
                    spacing: 1
                    Repeater {
                        model: dlg.e.attendeeList || []
                        RowLayout {
                            required property var modelData
                            Text { text: modelData.name; color: Theme.text; font.pixelSize: Theme.fontSmall + 1; elide: Text.ElideRight; Layout.maximumWidth: 300 }
                            Badge { text: modelData.status; fg: modelData.status === "accepted" ? Theme.good : modelData.status === "declined" ? Theme.bad : Theme.muted }
                        }
                    }
                }
                Label { text: qsTr("Reminder"); color: Theme.muted }
                ChoiceBox {
                    id: reminderBox
                    Layout.preferredWidth: 220
                    model: [qsTr("None"), qsTr("At the start"), qsTr("5 minutes before"), qsTr("10 minutes before"),
                            qsTr("15 minutes before"), qsTr("30 minutes before"), qsTr("1 hour before"),
                            qsTr("2 hours before"), qsTr("1 day before")]
                }
                Label { text: qsTr("Details"); color: Theme.muted }
                TextArea {
                    id: descriptionArea
                    Layout.fillWidth: true
                    Layout.minimumHeight: 80
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    enabled: dlg.mine
                    background: Rectangle { radius: Theme.radius; color: Theme.surface; border.color: Theme.border }
                }
                Text {
                    visible: !dlg.mine
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Theme.muted
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("%1 organizes this meeting; only the reminder is yours to change here. Answer from the invitation in your mail.").arg(dlg.e.organizerName || "")
                }
            }
        }
        // Wraps on a phone.
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 6
            layoutDirection: Qt.RightToLeft
            Button {
                text: qsTr("Save")
                highlighted: true
                onClicked: {
                    if (dlg.mine && dlg.hasAttendees && (dlg.uid ? dlg.e.sent : true)) sendAsk.open()
                    else dlg.save(false)
                }
            }
            Button { text: qsTr("Cancel"); flat: true; onClicked: dlg.close() }
            Button {
                visible: !!dlg.uid && dlg.mine && !!dlg.e.sent
                text: qsTr("Cancel meeting…")
                flat: true
                onClicked: cancelAsk.open()
            }
            Button {
                visible: !!dlg.uid && !(dlg.mine && !!dlg.e.sent)
                text: qsTr("Remove")
                flat: true
                onClicked: { MailApp.removeEvent(dlg.uid); dlg.saved(dlg.uid); dlg.close() }
            }
            Button {
                visible: !!dlg.uid
                text: qsTr("Export…")
                flat: true
                onClicked: { exportDialog.selectedFile = "file:///" + MailApp.suggestedEventFileName(dlg.uid); exportDialog.open() }
            }
        }
    }

    Dialog {
        id: sendAsk
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: dlg.uid && dlg.e.sent ? qsTr("Send the update?") : qsTr("Send the invitation?")
        Label {
            width: Math.min(360, Window.width - 96)
            wrapMode: Text.Wrap
            text: dlg.uid && dlg.e.sent
                  ? qsTr("The attendees get an updated invitation by email and are asked to answer again.")
                  : qsTr("The attendees get an invitation by email.")
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Send"); highlighted: true; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { text: qsTr("Save only"); flat: true; DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole }
            Button { text: qsTr("Back"); flat: true; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onAccepted: { sendAsk.close(); dlg.save(true) }
            onApplied: { sendAsk.close(); dlg.save(false) }
            onRejected: sendAsk.close()
        }
    }
    Dialog {
        id: cancelAsk
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Cancel the meeting?")
        Label {
            width: Math.min(360, Window.width - 96)
            wrapMode: Text.Wrap
            text: qsTr("The attendees are told by email that it is cancelled.")
        }
        footer: DialogButtonBox {
            Button { text: qsTr("Cancel meeting"); highlighted: true; DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole }
            Button { text: qsTr("Back"); flat: true; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
            onAccepted: { cancelAsk.close(); MailApp.cancelEvent(dlg.uid, true); dlg.saved(dlg.uid); dlg.close() }
            onRejected: cancelAsk.close()
        }
    }
    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ics"
        nameFilters: [qsTr("Calendar files (*.ics)")]
        onAccepted: MailApp.exportEvent(dlg.uid, selectedFile)
    }
    component RowText: Label {
        Layout.fillWidth: Theme.narrow
        wrapMode: Text.Wrap
    }
}
