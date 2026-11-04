// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A date typed as YYYY-MM-DD, or picked from a month grid.
RowLayout {
    id: row
    property string date: "" // ISO
    property bool valid: field.acceptableInput && !isNaN(Date.parse(field.text))
    spacing: 2
    Field {
        id: field
        Layout.fillWidth: true
        text: row.date
        placeholderText: "YYYY-MM-DD"
        inputMethodHints: Qt.ImhDate
        validator: RegularExpressionValidator { regularExpression: /\d{4}-\d{2}-\d{2}/ }
        onTextEdited: if (acceptableInput) row.date = text
        onEditingFinished: if (acceptableInput) row.date = text
    }
    IconButton {
        iconName: "calendar"
        tip: qsTr("Pick a date")
        onClicked: {
            const d = new Date(row.date + "T00:00:00")
            grid.year = isNaN(d) ? new Date().getFullYear() : d.getFullYear()
            grid.month = isNaN(d) ? new Date().getMonth() : d.getMonth()
            picker.open()
        }
        Popup {
            id: picker
            y: parent.height
            x: Qt.application.layoutDirection === Qt.RightToLeft ? parent.width - width : 0
            padding: 8
            modal: true
            background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: Theme.radius }
            contentItem: ColumnLayout {
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    IconButton { iconName: "back"; tip: qsTr("Previous month"); onClicked: { if (grid.month === 0) { grid.month = 11; grid.year-- } else grid.month-- } }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: Qt.locale().standaloneMonthName(grid.month, Locale.LongFormat) + " " + grid.year
                        color: Theme.text
                        font.weight: Font.DemiBold
                    }
                    IconButton { iconName: "forward"; tip: qsTr("Next month"); onClicked: { if (grid.month === 11) { grid.month = 0; grid.year++ } else grid.month++ } }
                }
                DayOfWeekRow {
                    Layout.fillWidth: true
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
                    locale: Qt.locale()
                    delegate: Rectangle {
                        required property var model
                        implicitWidth: 32; implicitHeight: 28
                        radius: Theme.radius
                        color: model.today ? Theme.selection : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: model.day
                            opacity: model.month === grid.month ? 1 : 0.35
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall + 1
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                const d = model.date
                                row.date = d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0")
                                field.text = row.date
                                picker.close()
                            }
                        }
                    }
                }
            }
        }
    }
}
