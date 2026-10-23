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
    // Beside a field: on a phone it wraps rather than push the row off screen.
    component RowText: Label {
        Layout.fillWidth: Theme.narrow
        wrapMode: Text.Wrap
    }
    component DaysField: SpinBox {
        from: -1; to: 3650
        editable: true
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: col.implicitHeight + 60
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: col
            readonly property int margin: Theme.narrow ? 16 : 32
            // Mirrored by hand: x is not.
            x: LayoutMirroring.enabled ? flick.width - width - margin : margin
            y: Theme.narrow ? 12 : 24
            width: Math.min(flick.width - 2 * margin, 640)
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
            CheckBox {
                visible: MailApp.backgroundSyncAvailable
                text: qsTr("Keep syncing in the background")
                checked: MailApp.backgroundSync
                onToggled: MailApp.backgroundSync = checked
            }
            Note {
                visible: MailApp.backgroundSyncAvailable
                text: qsTr("Mail and your other devices' changes keep arriving while e3mail is not in front. Android shows a notification for as long as this is on, and it uses more battery. Swiping e3mail away from the recent apps ends it.")
            }

            Heading { text: qsTr("You") }
            Label { text: qsTr("Name"); color: Theme.muted }
            Field {
                Layout.fillWidth: true
                text: view.s.displayName || ""
                onEditingFinished: MailApp.setSetting("displayName", text)
            }
            Label { text: qsTr("Signatures"); color: Theme.muted }
            ButtonGroup { id: defaultGroup }
            Repeater {
                model: MailApp.signatures
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4
                    RadioButton {
                        ButtonGroup.group: defaultGroup
                        checked: modelData.isDefault
                        onClicked: MailApp.defaultSignature = modelData.id
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Put this one in new mail")
                        Accessible.name: qsTr("Default: %1").arg(modelData.name)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; elide: Text.ElideRight }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.preview
                            color: Theme.muted
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    Button { text: qsTr("Edit"); flat: true; onClicked: signatureDialog.edit(modelData.id) }
                    IconButton { iconName: "trash"; tip: qsTr("Delete %1").arg(modelData.name); onClicked: MailApp.removeSignature(modelData.id) }
                }
            }
            RadioButton {
                visible: MailApp.signatures.length > 0
                ButtonGroup.group: defaultGroup
                checked: MailApp.defaultSignature === ""
                onClicked: MailApp.defaultSignature = ""
                text: qsTr("No signature in new mail")
            }
            Button { text: qsTr("Add a signature…"); onClicked: signatureDialog.edit("") }
            Note { text: qsTr("The chosen one goes into new mail. The composer's signature button swaps it for another.") }

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
                // Wraps on a phone instead of eliding or overflowing.
                Layout.fillWidth: true
                Component.onCompleted: { contentItem.wrapMode = Text.Wrap; contentItem.elide = Text.ElideNone }
                text: qsTr("Hold mail from senders I have not accepted in Unverified")
                checked: view.s.gating === true
                onToggled: MailApp.setSetting("gating", checked)
            }
            RowLayout {
                RowText { text: qsTr("Move unaccepted mail to Trash after"); color: Theme.text }
                DaysField { from: 0; value: view.s.unverifiedTrashDays || 0; onValueModified: MailApp.setSetting("unverifiedTrashDays", value) }
                RowText { text: qsTr("days (0: never)"); color: Theme.muted }
            }

            Heading { text: qsTr("Read receipts") }
            ChoiceBox {
                Layout.fillWidth: true
                model: [{ v: "trusted", t: qsTr("Send a read receipt to trusted senders who ask for one") },
                        { v: "never", t: qsTr("Never send read receipts") }]
                textRole: "t"; valueRole: "v"
                selected: view.s.mdnSend === "never" ? 1 : 0
                onActivated: MailApp.setSetting("mdnSend", currentValue)
            }
            Note { text: qsTr("A receipt goes once, when you first open the message, on whichever device. Strangers and anyone you do not trust never get one.") }

            Heading { text: qsTr("Trash") }
            RowLayout {
                RowText { text: qsTr("Destroy messages in Trash after"); color: Theme.text }
                DaysField { from: 0; value: view.s.trashPurgeDays || 0; onValueModified: MailApp.setSetting("trashPurgeDays", value) }
                RowText { text: qsTr("days (0: at once)"); color: Theme.muted }
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
                RowText { text: qsTr("Keep for"); color: Theme.text }
                DaysField { from: 1; value: view.s.serverKeepDays || 30; onValueModified: MailApp.setSetting("serverKeepDays", value) }
                RowText { text: qsTr("days"); color: Theme.muted }
            }
            Note { text: qsTr("Mail that was on the server before e3mail first connected is never removed. \"Never\" lets another mail program keep using this mailbox.") }
            RowLayout {
                visible: view.s.protocol === "pop3"
                RowText { text: qsTr("Check for mail every"); color: Theme.text }
                SpinBox { from: 30; to: 3600; stepSize: 30; editable: true; value: view.s.pollSeconds || 300; onValueModified: MailApp.setSetting("pollSeconds", value) }
                RowText { text: qsTr("seconds"); color: Theme.muted }
            }
            // The worker refused a certificate: not trusted, or not the one
            // pinned (ADR 0017).
            Rectangle {
                Layout.fillWidth: true
                visible: MailApp.refusedCertificate.fingerprint !== undefined
                radius: Theme.radius
                color: Theme.badBg
                implicitHeight: refusedCol.implicitHeight + 20
                ColumnLayout {
                    id: refusedCol
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.bad
                        text: MailApp.refusedCertificate.server === "smtp"
                              ? qsTr("The sending server's certificate is not trusted, or is not the one trusted before.")
                              : qsTr("The incoming server's certificate is not trusted, or is not the one trusted before.")
                    }
                    TextEdit {
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        color: Theme.text
                        font.family: "monospace"
                        font.pixelSize: Theme.fontSmall + 1
                        text: MailApp.refusedCertificate.fingerprint || ""
                    }
                    Note { text: qsTr("Trust it only if this SHA-256 fingerprint matches the one your server's administrator gave you.") }
                    Button { text: qsTr("Trust this certificate"); onClicked: { MailApp.trustRefusedCertificate(); view.reload() } }
                }
            }
            Repeater {
                model: [{ k: "in", f: view.s.inCert || "" }, { k: "smtp", f: view.s.smtpCert || "" }]
                RowLayout {
                    required property var modelData
                    visible: modelData.f.length > 0
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        color: Theme.muted
                        font.pixelSize: Theme.fontSmall + 1
                        text: (modelData.k === "smtp" ? qsTr("Sending server: trusted certificate %1")
                                                      : qsTr("Incoming server: trusted certificate %1")).arg(modelData.f)
                    }
                    Button { text: qsTr("Forget"); flat: true; onClicked: { MailApp.forgetCertificate(modelData.k); view.reload() } }
                }
            }
            Note {
                text: qsTr("%1 %2:%3 · SMTP %4:%5 · password stored in the %6")
                      .arg((view.s.protocol || "").toUpperCase()).arg(view.s.inHost || "").arg(view.s.inPort || "")
                      .arg(view.s.smtpHost || "").arg(view.s.smtpPort || "")
                      .arg(view.s.secretLocation === "keyring" ? qsTr("system keyring") : qsTr("account database (no system keyring was available)"))
            }

            Heading { text: qsTr("On this device") }
            Note {
                text: view.s.atRest === "passphrase" ? qsTr("Your mail here is encrypted, and your passphrase opens it. Without it, nobody can read it, and neither can you.")
                    : view.s.atRest === "keyring" ? qsTr("Your mail here is encrypted with a key kept in the system keyring. Anyone who can sign in to this computer as you can read it.")
                    : qsTr("Your mail here is encrypted, but its key is stored beside it (no system keyring was available). Set a passphrase to protect it.")
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Button { text: (view.s.atRest === "passphrase") ? qsTr("Change passphrase…") : qsTr("Set a passphrase…"); onClicked: passphraseDialog.openFor(true) }
                Button { visible: (view.s.atRest === "passphrase"); flat: true; text: qsTr("Remove passphrase…"); onClicked: passphraseDialog.openFor(false) }
            }

            Heading { text: qsTr("Originals") }
            RowLayout {
                RowText { text: qsTr("Keep each message's original for"); color: Theme.text }
                DaysField { value: view.s.rawMimeDays !== undefined ? view.s.rawMimeDays : 30; onValueModified: MailApp.setSetting("rawMimeDays", value) }
                RowText { text: qsTr("days (0: never, −1: forever)"); color: Theme.muted }
            }

            Heading { text: qsTr("Blocked") }
            Repeater {
                id: blocked
                RowLayout {
                    required property string modelData
                    RowText { text: modelData; color: Theme.text }
                    Button { text: qsTr("Unblock"); flat: true; onClicked: { MailApp.unblock(modelData); view.reload() } }
                }
            }
            RowLayout {
                Field { id: blockField; Layout.preferredWidth: 260; placeholderText: qsTr("someone@example.com or @example.com") }
                Button { text: qsTr("Block"); onClicked: { MailApp.block(blockField.text); blockField.text = ""; view.reload() } }
            }

            Heading { id: devicesHeading; text: qsTr("Devices") }
            Note {
                visible: !MailApp.devicesAvailable
                text: qsTr("This build of e3mail cannot sync with other devices.")
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: MailApp.devicesAvailable
                spacing: 6
                // Removed on another device while this one was away.
                Rectangle {
                    Layout.fillWidth: true
                    visible: MailApp.removedBy.length > 0
                    implicitHeight: removedRow.implicitHeight + 16
                    radius: Theme.radius
                    color: Theme.badBg
                    border.color: Qt.alpha(Theme.bad, 0.4)
                    RowLayout {
                        id: removedRow
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            color: Theme.text
                            text: qsTr("“%1” removed this device from the account, so they no longer sync. Mail still arrives here from the server.").arg(MailApp.removedBy)
                        }
                        Button { text: qsTr("OK"); flat: true; onClicked: MailApp.dismissRemoved() }
                    }
                }
                Label { text: qsTr("This device"); color: Theme.muted }
                Field {
                    Layout.fillWidth: true
                    text: MailApp.deviceName
                    placeholderText: MailApp.defaultDeviceName()
                    onEditingFinished: MailApp.deviceName = text
                }
                Repeater {
                    model: MailApp.devices
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle { width: 8; height: 8; radius: 4; color: modelData.online ? Theme.good : Theme.muted }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; elide: Text.ElideRight }
                            Label {
                                Layout.fillWidth: true
                                color: Theme.muted
                                font.pixelSize: Theme.fontSmall
                                elide: Text.ElideRight
                                text: modelData.online ? qsTr("Connected")
                                      : modelData.lastSeen.length ? qsTr("Last seen %1").arg(modelData.lastSeen)
                                      : qsTr("Not seen yet")
                            }
                        }
                        Button { text: qsTr("Remove"); flat: true; onClicked: { removeDeviceConfirm.device = modelData; removeDeviceConfirm.open() } }
                    }
                }
                Button { text: qsTr("Pair a new device…"); onClicked: pairDialog.begin() }
                Label { text: qsTr("Relay"); color: Theme.muted }
                ChoiceBox {
                    id: relayBox
                    Layout.fillWidth: true
                    model: [{ v: "default", t: qsTr("n0's public relays") },
                            { v: "custom", t: qsTr("A relay I run") },
                            { v: "off", t: qsTr("None: only on the same network") }]
                    textRole: "t"; valueRole: "v"
                    selected: MailApp.relay === "default" || MailApp.relay === "off" ? ["default", "custom", "off"].indexOf(MailApp.relay) : 1
                    onActivated: { if (currentValue !== "custom") MailApp.relay = currentValue; else relayUrl.forceActiveFocus() }
                }
                Field {
                    id: relayUrl
                    Layout.fillWidth: true
                    visible: relayBox.currentValue === "custom"
                    placeholderText: "https://relay.example.org"
                    text: MailApp.relay.startsWith("http") ? MailApp.relay : ""
                    onEditingFinished: if (text.startsWith("https://") || text.startsWith("http://")) MailApp.relay = text
                }
                Note { text: qsTr("Devices of this account sync directly with each other, end-to-end encrypted. When they cannot reach each other, a relay passes the encrypted traffic along; it cannot read it.") }
            }

            Heading { id: backupsHeading; text: qsTr("Backups") }
            BackupController { id: backups }
            ColumnLayout {
                Layout.fillWidth: true
                visible: backups.isSetUp
                spacing: 6
                Note { text: qsTr("Encrypted, to %1, once a day while e3mail runs.").arg(backups.where) }
                Note {
                    text: backups.busy ? qsTr("Backing up…")
                        : backups.lastError.length ? qsTr("The last backup failed: %1").arg(backups.lastError)
                        : backups.last.length ? qsTr("Last backup: %1, checked by reading it back.").arg(backups.last)
                        : qsTr("No backup yet.")
                    color: backups.lastError.length && !backups.busy ? Theme.bad : Theme.muted
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Button { text: qsTr("Back up now"); enabled: !backups.busy; onClicked: backups.backUpNow() }
                    Button { text: qsTr("Stop backing up"); flat: true; onClicked: backups.stop() }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: !backups.isSetUp
                spacing: 6
                Note { text: qsTr("Back this mailbox up, encrypted, to a place of your choice. The place sees only encrypted data and its size.") }
                BackupForm { id: backupForm; Layout.fillWidth: true; controller: backups; purpose: "setup" }
                RowLayout {
                    PrimaryButton {
                        text: backups.busy ? qsTr("Setting up…") : qsTr("Set up backups")
                        enabled: !backups.busy && backupForm.ready()
                        onClicked: backups.setUp(backupForm.destination(), backupForm.secret(), backupForm.passphrase())
                    }
                    BusyIndicator { running: backups.busy; visible: running; implicitWidth: 28; implicitHeight: 28 }
                }
            }

            Heading { text: qsTr("Data") }
            Note { text: qsTr("Your mail lives only here: %1. Back this folder up — the server keeps no copy.").arg(view.s.dataDir || "") }
            Note { visible: MailApp.portable; text: qsTr("Portable mode: data is kept beside the program.") }

            Heading { text: qsTr("Accounts") }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Button { text: qsTr("Add a mailbox…"); onClicked: view.addAccount() }
                Button { text: qsTr("Remove this mailbox…"); flat: true; onClicked: removeConfirm.open() }
            }
            Note { text: qsTr("e3mail %1 · unaudited development software").arg(MailApp.version) }
        }
    }

    SignatureDialog { id: signatureDialog }
    PairDialog { id: pairDialog }

    // Setting, changing or removing the passphrase (ADR 0023).
    Dialog {
        id: passphraseDialog
        property bool setting: true
        function openFor(set) {
            setting = set
            currentPass.text = ""; newPass.text = ""; repeatPass.text = ""; passError.text = ""
            open()
        }
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(440, parent.width - (Theme.narrow ? 16 : 96))
        title: setting ? qsTr("Passphrase") : qsTr("Remove passphrase")
        standardButtons: Dialog.Cancel
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: passphraseDialog.setting
                      ? qsTr("e3mail will ask for it each time it starts. If you forget it, the mail on this device cannot be recovered; a backup or another device can restore it.")
                      : qsTr("Your mail stays encrypted; its key goes back to the system keyring, or beside the mail if there is none.")
            }
            Field { id: currentPass; visible: (view.s.atRest === "passphrase"); Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("Current passphrase") }
            Field { id: newPass; visible: passphraseDialog.setting; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("New passphrase") }
            Field { id: repeatPass; visible: passphraseDialog.setting; Layout.fillWidth: true; echoMode: TextInput.Password; placeholderText: qsTr("New passphrase again") }
            Label { id: passError; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.bad; visible: text.length > 0 }
            PrimaryButton {
                Layout.alignment: Qt.AlignRight
                text: passphraseDialog.setting ? qsTr("Save") : qsTr("Remove")
                onClicked: {
                    if (passphraseDialog.setting && newPass.text.length < 8) { passError.text = qsTr("Use at least 8 characters."); return }
                    if (passphraseDialog.setting && newPass.text !== repeatPass.text) { passError.text = qsTr("The two do not match."); return }
                    const err = MailApp.setPassphrase(currentPass.text, passphraseDialog.setting ? newPass.text : "")
                    if (err.length) { passError.text = err; return }
                    passphraseDialog.close()
                    view.reload()
                }
            }
        }
    }
    function pairDevice() { pairDialog.begin() }
    // After layout, which has not happened when the page has just opened.
    function showDevices() { scrollToDevices.start() }
    // For screenshots (--page backups).
    function showBackups() { scrollToBackups.start() }
    Timer { id: scrollToBackups; interval: 300; onTriggered: flick.contentY = Math.max(0, backupsHeading.y - 12) }
    Timer { id: scrollToDevices; interval: 300; onTriggered: flick.contentY = Math.max(0, devicesHeading.y - 12) }
    Dialog {
        id: removeDeviceConfirm
        property var device: ({})
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Remove %1?").arg(device.name || "")
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: Math.min(implicitWidth, Window.width - 96)
            wrapMode: Text.Wrap
            text: qsTr("It stops syncing with this account's other devices. What is already on it stays there.")
        }
        onAccepted: MailApp.removeDevice(device.id)
    }
    function editSignature(id) { signatureDialog.edit(id) }

    Dialog {
        id: removeConfirm
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Remove %1?").arg(view.s.addr || "")
        standardButtons: Dialog.Yes | Dialog.Cancel
        Label {
            width: Math.min(implicitWidth, Window.width - 96)
            wrapMode: Text.Wrap
            text: qsTr("Every message, contact and key of this mailbox is deleted from this device.")
        }
        onAccepted: MailApp.removeAccount(MailApp.currentAccountId)
    }
}
