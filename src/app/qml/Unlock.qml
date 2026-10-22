// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Asks for the passphrase that opens every account, before anything is
// opened (ADR 0023). `Gate` is set by main.cpp.
ApplicationWindow {
    id: win
    width: 420
    height: 260
    visible: true
    title: "e3mail"
    color: Theme.window
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    function tryOpen() {
        if (!field.text.length) return
        busy.running = true
        // Let the busy indicator show: deriving the key takes a moment.
        Qt.callLater(function() {
            const ok = Gate.unlock(field.text)
            busy.running = false
            if (!ok) { wrong.visible = true; field.selectAll(); field.forceActiveFocus() }
        })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12
        Label {
            Layout.fillWidth: true
            text: qsTr("Enter your passphrase")
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.muted
            text: qsTr("Your mail on this device is encrypted with it.")
        }
        Field {
            id: field
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: qsTr("Passphrase")
            focus: true
            onAccepted: win.tryOpen()
            onTextChanged: wrong.visible = false
        }
        Label {
            id: wrong
            visible: false
            color: Theme.bad
            text: qsTr("That passphrase does not open your mail.")
        }
        RowLayout {
            Layout.fillWidth: true
            BusyIndicator { id: busy; running: false; visible: running; implicitWidth: 28; implicitHeight: 28 }
            Item { Layout.fillWidth: true }
            PrimaryButton { text: qsTr("Open"); enabled: field.text.length > 0 && !busy.running; onClicked: win.tryOpen() }
        }
        Item { Layout.fillHeight: true }
    }
}
