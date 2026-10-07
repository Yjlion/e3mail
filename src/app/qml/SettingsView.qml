// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: view
    color: Theme.window
    signal addAccount()
    property var s: ({})

    function reload() { s = MailApp.settings(); blocked.model = MailApp.blocklist() }
    onVisibleChanged: if (visible) reload()
    Connections { target: MailApp; function onAccountChanged() { view.reload() } }

    component Heading: Text {
        Layout.topMargin: 22
        color: Theme.text
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.muted
        font.pixelSize: Theme.fontSmall + 1
    }
    component DaysField: SpinBox {
        from: -1; to: 3650
        editable: true
    }

    Flickable {
        anchors.fill: parent
        contentHeight: col.implicitHeight + 60
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: col
            // Mirrored by hand: x is not.
            x: LayoutMirroring.enabled ? parent.width - width - 32 : 32
            y: 24
            width: Math.min(parent.width - 64, 640)
            spacing: 6

            Text { text: qsTr("Settings"); color: Theme.text; font.pixelSize: 24; font.weight: Font.DemiBold }
            Note { text: view.s.addr || "" }

            Heading { text: qsTr("Language") }
            ComboBox {
                id: languageBox
                Layout.preferredWidth: 300
                textRole: "name"
                valueRole: "code"
                model: [{ code: "", name: qsTr("Same as the system") }].concat(MailApp.languages)
                // Retranslating rebuilds the model; keep the choice shown.
                function showChoice() { currentIndex = indexOfValue(MailApp.language) }
                Component.onCompleted: showChoice()
                onModelChanged: Qt.callLater(showChoice)
                onActivated: MailApp.language = currentValue
            }
            Note { text: qsTr("Translations other than English were made by machine and await review by native speakers.") }

            Heading { text: qsTr("Notifications") }
            ChoiceBox {
                Layout.fillWidth: true
                enabled: MailApp.notificationsAvailable
                model: [{ v: "off", t: qsTr("Off") },
                        { v: "sender", t: qsTr("Show who wrote") },
                        { v: "full", t: qsTr("Show who wrote and the subject") }]
                textRole: "t"; valueRole: "v"
                selected: ["off", "sender", "full"].indexOf(MailApp.notificationMode)
                onActivated: MailApp.notificationMode = currentValue
            }
            Note {
                text: MailApp.notificationsAvailable
                      ? qsTr("For this device, while e3mail is open and not in front. A subject shown here is kept in the system's notification history, outside e3mail's encryption.")
                      : qsTr("This system offers no desktop notifications.")
            }

            Heading { text: qsTr("You") }
            Label { text: qsTr("Name"); color: Theme.muted }
            Field {
                Layout.fillWidth: true
                text: view.s.displayName || ""
                onEditingFinished: MailApp.setSetting("displayName", text)
            }
            Label { text: qsTr("Signature"); color: Theme.muted }
            TextArea {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                text: view.s.signature || ""
                color: Theme.text
                wrapMode: TextEdit.Wrap
                onEditingFinished: MailApp.setSetting("signature", text)
                background: Rectangle { radius: Theme.radius; color: Theme.surface; border.color: Theme.border }
            }

            Heading { text: qsTr("Encryption") }
            ChoiceBox {
                Layout.fillWidth: true
                model: [qsTr("Lenient — encrypt when everyone asks for it"),
                        qsTr("Opportunistic — encrypt whenever every recipient has a key"),
                        qsTr("Strict — end-to-end only, refuse otherwise")]
                selected: view.s.encryptionMode !== undefined ? view.s.encryptionMode : 1
                onActivated: (i) => MailApp.setSetting("encryptionMode", i)
            }
            Note {
                text: qsTr("Keys are learned from the Autocrypt header other clients send. A learned key encrypts, but only verifying a contact in person proves who holds it. Your key: %1").arg(view.s.fingerprint || "")
            }

            Heading { text: qsTr("Strangers") }
            CheckBox {
                text: qsTr("Hold mail from senders I have not accepted in Unverified")
                checked: view.s.gating === true
                onToggled: MailApp.setSetting("gating", checked)
            }
            RowLayout {
                Label { text: qsTr("Move unaccepted mail to Trash after"); color: Theme.text }
                DaysField { from: 0; value: view.s.unverifiedTrashDays || 0; onValueModified: MailApp.setSetting("unverifiedTrashDays", value) }
                Label { text: qsTr("days (0: never)"); color: Theme.muted }
            }

            Heading { text: qsTr("Trash") }
            RowLayout {
                Label { text: qsTr("Destroy messages in Trash after"); color: Theme.text }
                DaysField { from: 0; value: view.s.trashPurgeDays || 0; onValueModified: MailApp.setSetting("trashPurgeDays", value) }
                Label { text: qsTr("days (0: at once)"); color: Theme.muted }
            }
            Note { text: qsTr("Trash is the only place e3mail destroys mail on a timer. Unaccepted and blocked mail goes there first.") }

            Heading { text: qsTr("Server") }
            ChoiceBox {
                Layout.fillWidth: true
                model: [{ v: "delete", t: qsTr("Remove mail from the server after downloading") },
                        { v: "keep", t: qsTr("Keep mail on the server for some days") },
                        { v: "never", t: qsTr("Never remove mail from the server") }]
                textRole: "t"; valueRole: "v"
                selected: ["delete", "keep", "never"].indexOf(view.s.serverRetention || "delete")
                onActivated: { MailApp.setSetting("serverRetention", currentValue); view.reload() }
            }
            RowLayout {
                visible: view.s.serverRetention === "keep"
                Label { text: qsTr("Keep for"); color: Theme.text }
                DaysField { from: 1; value: view.s.serverKeepDays || 30; onValueModified: MailApp.setSetting("serverKeepDays", value) }
                Label { text: qsTr("days"); color: Theme.muted }
            }
            Note { text: qsTr("Mail that was on the server before e3mail first connected is never removed. \"Never\" lets another mail program keep using this mailbox.") }
            RowLayout {
                visible: view.s.protocol === "pop3"
                Label { text: qsTr("Check for mail every"); color: Theme.text }
                SpinBox { from: 30; to: 3600; stepSize: 30; editable: true; value: view.s.pollSeconds || 300; onValueModified: MailApp.setSetting("pollSeconds", value) }
                Label { text: qsTr("seconds"); color: Theme.muted }
            }
            Note {
                text: qsTr("%1 %2:%3 · SMTP %4:%5 · password stored in the %6")
                      .arg((view.s.protocol || "").toUpperCase()).arg(view.s.inHost || "").arg(view.s.inPort || "")
                      .arg(view.s.smtpHost || "").arg(view.s.smtpPort || "")
                      .arg(view.s.secretLocation === "keyring" ? qsTr("system keyring") : qsTr("account database (no system keyring was available)"))
            }

            Heading { text: qsTr("Originals") }
            RowLayout {
                Label { text: qsTr("Keep each message's original for"); color: Theme.text }
                DaysField { value: view.s.rawMimeDays !== undefined ? view.s.rawMimeDays : 30; onValueModified: MailApp.setSetting("rawMimeDays", value) }
                Label { text: qsTr("days (0: never, −1: forever)"); color: Theme.muted }
            }

            Heading { text: qsTr("Blocked") }
            Repeater {
                id: blocked
                RowLayout {
                    required property string modelData
                    Label { text: modelData; color: Theme.text }
                    Button { text: qsTr("Unblock"); flat: true; onClicked: { MailApp.unblock(modelData); view.reload() } }
                }
            }
            RowLayout {
                Field { id: blockField; Layout.preferredWidth: 260; placeholderText: qsTr("someone@example.com or @example.com") }
                Button { text: qsTr("Block"); onClicked: { MailApp.block(blockField.text); blockField.text = ""; view.reload() } }
            }

            Heading { text: qsTr("Data") }
            Note { text: qsTr("Your mail lives only here: %1. Back this folder up — the server keeps no copy.").arg(view.s.dataDir || "") }
            Note { visible: MailApp.portable; text: qsTr("Portable mode: data is kept beside the program.") }

            Heading { text: qsTr("Accounts") }
            RowLayout {
                Button { text: qsTr("Add a mailbox…"); onClicked: view.addAccount() }
                Button { text: qsTr("Remove this mailbox…"); flat: true; onClicked: removeConfirm.open() }
            }
            Note { text: qsTr("e3mail %1 · unaudited development software").arg(MailApp.version) }
        }
    }

    Dialog {
        id: removeConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Remove %1?").arg(view.s.addr || "")
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label { text: qsTr("Every message, contact and key of this mailbox is deleted from this device.") }
        onAccepted: MailApp.removeAccount(MailApp.currentAccountId)
    }
}
