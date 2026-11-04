// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A meeting invitation, an answer to one, or a cancellation (ADR 0025): the
// facts as plain text, and the answers only for a trusted sender who is the
// meeting's organizer. Nothing enters the calendar until the person acts.
Rectangle {
    id: card
    property var m: ({})
    readonly property var inv: m.invitation || ({})
    signal showEvent(string uid)
    signal askOpenLink(string link)

    visible: !!inv.uid
    radius: Theme.radius
    color: Theme.raised
    border.color: Theme.border
    implicitHeight: col.implicitHeight + 20

    function statusText(s) {
        if (s === "accepted") return qsTr("Accepted")
        if (s === "tentative") return qsTr("Tentative")
        if (s === "declined") return qsTr("Declined")
        return qsTr("Not answered")
    }

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 10
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Image {
                source: Theme.icon("calendar")
                sourceSize: Qt.size(18, 18)
                opacity: 0.7
            }
            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                font.pixelSize: Theme.fontBody + 1
                font.weight: Font.DemiBold
                color: Theme.text
                wrapMode: Text.Wrap
                text: card.inv.method === "cancel" ? qsTr("Cancelled: %1").arg(card.inv.title || "") : (card.inv.title || "")
            }
        }
        // What happened, in a sentence.
        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontSmall + 1
            text: card.inv.method === "reply"
                      ? (card.inv.replyStatus === "accepted" ? qsTr("%1 accepted.").arg(card.inv.replyFrom)
                         : card.inv.replyStatus === "declined" ? qsTr("%1 declined.").arg(card.inv.replyFrom)
                         : card.inv.replyStatus === "tentative" ? qsTr("%1 might come.").arg(card.inv.replyFrom)
                         : qsTr("%1 answered.").arg(card.inv.replyFrom))
                  : card.inv.method === "cancel"
                      ? (card.inv.inCalendar ? qsTr("This meeting was cancelled; your calendar is updated.") : qsTr("This meeting was cancelled."))
                  : card.inv.mine ? qsTr("Your invitation.")
                  : card.inv.outdated ? qsTr("A newer version of this invitation is in your calendar.")
                  : card.inv.cancelled ? qsTr("This meeting has since been cancelled.")
                  : card.inv.inCalendar ? qsTr("In your calendar: %1.").arg(card.statusText(card.inv.myStatus).toLowerCase())
                  : qsTr("%1 invites you.").arg(card.inv.organizer)
        }
        GridLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            columns: 2
            columnSpacing: 8
            rowSpacing: 2
            component FactLabel: Text {
                textFormat: Text.PlainText
                color: Theme.muted
                font.pixelSize: Theme.fontSmall + 1
                Layout.preferredWidth: 110
                elide: Text.ElideRight
            }
            component FactValue: Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.fontSmall + 1
                wrapMode: Text.Wrap
            }
            FactLabel { text: qsTr("When") }
            FactValue { text: card.inv.when || "" }
            FactLabel { visible: !!card.inv.recurrence; text: qsTr("Repeats") }
            FactValue { visible: !!card.inv.recurrence; text: card.inv.recurrence || "" }
            FactLabel { visible: !!card.inv.where; text: qsTr("Where") }
            FactValue { visible: !!card.inv.where; text: card.inv.where || "" }
            FactLabel { visible: !!card.inv.organizer && card.inv.method !== "reply"; text: qsTr("Organizer") }
            FactValue { visible: !!card.inv.organizer && card.inv.method !== "reply"; text: card.inv.organizer || "" }
            FactLabel { visible: (card.inv.attendees || []).length > 0 && card.inv.method !== "reply"; text: qsTr("Who"); Layout.alignment: Qt.AlignTop }
            ColumnLayout {
                visible: (card.inv.attendees || []).length > 0 && card.inv.method !== "reply"
                Layout.fillWidth: true
                spacing: 1
                Repeater {
                    model: card.inv.attendees || []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: modelData.name
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall + 1
                            elide: Text.ElideRight
                            Layout.maximumWidth: 260
                        }
                        Badge {
                            visible: card.inv.mine || card.inv.inCalendar
                            text: card.statusText(modelData.status)
                            fg: modelData.status === "accepted" ? Theme.good : modelData.status === "declined" ? Theme.bad : Theme.muted
                        }
                    }
                }
            }
            FactLabel { visible: !!card.inv.description; text: qsTr("Details"); Layout.alignment: Qt.AlignTop }
            FactValue { visible: !!card.inv.description; text: card.inv.description || "" }
        }
        // Answering: only an active invitation. The chosen answer stays lit.
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 6
            visible: card.inv.method === "request" && !card.inv.mine && !card.inv.outdated && !card.inv.cancelled
            Button {
                visible: !!card.inv.active
                text: qsTr("Accept")
                highlighted: card.inv.myStatus === "accepted"
                onClicked: MailApp.replyToInvitation(card.m.id, "accept")
            }
            Button {
                visible: !!card.inv.active
                text: qsTr("Tentative")
                highlighted: card.inv.myStatus === "tentative"
                onClicked: MailApp.replyToInvitation(card.m.id, "tentative")
            }
            Button {
                visible: !!card.inv.active
                text: qsTr("Decline")
                highlighted: card.inv.myStatus === "declined"
                onClicked: MailApp.replyToInvitation(card.m.id, "decline")
            }
            Button {
                visible: !card.inv.active && !!card.inv.canAdd && !card.inv.inCalendar
                text: qsTr("Add to calendar")
                onClicked: MailApp.addInvitationToCalendar(card.m.id)
            }
            Button {
                visible: !!card.inv.inCalendar
                flat: true
                text: qsTr("Show in calendar")
                onClicked: card.showEvent(card.inv.uid)
            }
            Button {
                visible: !!card.inv.url
                flat: true
                text: qsTr("Open link…")
                onClicked: card.askOpenLink(card.inv.url)
            }
        }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: 6
            visible: (card.inv.method === "cancel" || card.inv.method === "publish") && !card.inv.mine
            Button {
                visible: !!card.inv.canAdd && !card.inv.inCalendar && card.inv.method === "publish"
                text: qsTr("Add to calendar")
                onClicked: MailApp.addInvitationToCalendar(card.m.id)
            }
            Button {
                visible: !!card.inv.inCalendar
                flat: true
                text: qsTr("Show in calendar")
                onClicked: card.showEvent(card.inv.uid)
            }
        }
        Text {
            Layout.fillWidth: true
            visible: card.inv.method === "request" && !card.inv.active && !card.inv.mine && !card.inv.outdated && !card.inv.cancelled
            wrapMode: Text.Wrap
            color: Theme.muted
            font.pixelSize: Theme.fontSmall
            text: card.inv.organizerIsSender
                  ? qsTr("Answers can be sent to senders you trust; accept this sender first. Check these details against the message.")
                  : qsTr("This message was not sent by the meeting's organizer, so it cannot be answered from here. Check these details against the message.")
        }
    }
}
