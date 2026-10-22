// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import E3mail

// What a message says about itself in structured data (ADR 0024): a flight,
// a booking, a parcel. Facts as plain text; the buttons only for a trusted
// sender whose message is signed, otherwise a note says why there are none.
ColumnLayout {
    id: sc
    property var m: ({})
    signal askOpenLink(string link)
    spacing: 8
    visible: (m.structured || []).length > 0

    Repeater {
        model: sc.m.structured || []
        Rectangle {
            id: item
            required property var modelData
            required property int index
            Layout.fillWidth: true
            radius: Theme.radius
            color: Theme.raised
            border.color: Theme.border
            implicitHeight: col.implicitHeight + 20
            ColumnLayout {
                id: col
                anchors.fill: parent
                anchors.margins: 10
                spacing: 4
                Text {
                    Layout.fillWidth: true
                    text: item.modelData.title
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.fontBody + 1
                    font.weight: Font.DemiBold
                    color: Theme.text
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: item.modelData.fields
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            text: modelData.label
                            textFormat: Text.PlainText
                            color: Theme.muted
                            font.pixelSize: Theme.fontSmall + 1
                            Layout.preferredWidth: 110
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.value
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall + 1
                            wrapMode: Text.Wrap
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 6
                    visible: sc.m.structuredActive && (item.modelData.calendar || item.modelData.link.length > 0)
                    Button {
                        visible: item.modelData.calendar
                        text: qsTr("Add to calendar…")
                        onClicked: {
                            icsDialog.index = item.index
                            icsDialog.selectedFile = "file:///" + item.modelData.title.replace(/[\/\\:*?"<>|]/g, "_") + ".ics"
                            icsDialog.open()
                        }
                    }
                    Button {
                        visible: item.modelData.link.length > 0
                        text: qsTr("Open…")
                        onClicked: sc.askOpenLink(item.modelData.link)
                    }
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: !sc.m.structuredActive
        wrapMode: Text.Wrap
        color: Theme.muted
        font.pixelSize: Theme.fontSmall
        text: qsTr("Shown as text only. Buttons appear for senders you trust whose mail is signed by their key; check these details against the message.")
    }

    FileDialog {
        id: icsDialog
        property int index: -1
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Calendar files (*.ics)")]
        onAccepted: MailApp.saveCalendarEvent(sc.m.id, index, selectedFile)
    }
}
