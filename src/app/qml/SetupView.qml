// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

Rectangle {
    id: view
    property bool cancellable: false
    signal finished()
    signal cancelled()
    color: Theme.window

    SetupController {
        id: setup
        onDone: view.finished()
    }

    Flickable {
        anchors.fill: parent
        contentHeight: form.implicitHeight + 80
        clip: true

        ColumnLayout {
            id: form
            width: Math.min(parent.width - 48, 520)
            x: (parent.width - width) / 2
            y: 48
            spacing: 10

            Text {
                text: qsTr("Add a mailbox")
                color: Theme.text
                font.pixelSize: 24
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Theme.muted
                font.pixelSize: Theme.fontBody
                text: qsTr("e3mail downloads your mail, keeps it on this device, and by default removes it from the server. A dedicated address is recommended. Mail already on the server when you add it is never deleted.")
            }

            Label { text: qsTr("Email address"); color: Theme.muted }
            Field {
                Layout.fillWidth: true
                text: setup.addr
                placeholderText: "you@example.com"
                inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                onTextChanged: setup.addr = text
                onEditingFinished: setup.suggest()
            }
            Label { text: qsTr("Password"); color: Theme.muted }
            Field {
                Layout.fillWidth: true
                echoMode: TextInput.Password
                text: setup.password
                onTextChanged: setup.password = text
                onAccepted: setup.submit()
            }
            Label { text: qsTr("Your name (optional)"); color: Theme.muted }
            Field { Layout.fillWidth: true; text: setup.displayName; onTextChanged: setup.displayName = text }

            Label { text: qsTr("Receive mail with"); color: Theme.muted; Layout.topMargin: 6 }
            RowLayout {
                RadioButton {
                    text: qsTr("IMAP")
                    checked: setup.protocol === "imap"
                    onClicked: setup.useProtocol("imap")
                }
                RadioButton {
                    text: qsTr("POP3")
                    checked: setup.protocol === "pop3"
                    onClicked: setup.useProtocol("pop3")
                }
            }

            Text {
                Layout.fillWidth: true
                visible: setup.suggestionNote.length > 0
                text: setup.suggestionNote
                color: Theme.muted
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSmall + 1
            }

            Text {
                text: setup.showServers ? qsTr("Hide server settings") : qsTr("Server settings…")
                color: Theme.accent
                font.pixelSize: Theme.fontSmall + 1
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { setup.suggest(); setup.showServers = !setup.showServers } }
            }

            GridLayout {
                Layout.fillWidth: true
                visible: setup.showServers
                columns: 3
                columnSpacing: 8
                rowSpacing: 6
                Label { text: setup.protocol === "pop3" ? "POP3" : "IMAP"; color: Theme.muted }
                Field { Layout.fillWidth: true; text: setup.inHost; onTextChanged: setup.inHost = text; placeholderText: qsTr("server") }
                Field { Layout.preferredWidth: 70; text: setup.inPort; onTextChanged: setup.inPort = parseInt(text) || 0; validator: IntValidator { bottom: 1; top: 65535 } }
                Item {}
                ChoiceBox {
                    Layout.columnSpan: 2
                    model: [{ v: "ssl", t: qsTr("TLS") }, { v: "starttls", t: "STARTTLS" }, { v: "plain", t: qsTr("None (insecure)") }]
                    textRole: "t"; valueRole: "v"
                    selected: ["ssl", "starttls", "plain"].indexOf(setup.inSecurity)
                    onActivated: setup.inSecurity = currentValue
                }
                Label { text: "SMTP"; color: Theme.muted }
                Field { Layout.fillWidth: true; text: setup.smtpHost; onTextChanged: setup.smtpHost = text; placeholderText: qsTr("server") }
                Field { Layout.preferredWidth: 70; text: setup.smtpPort; onTextChanged: setup.smtpPort = parseInt(text) || 0; validator: IntValidator { bottom: 1; top: 65535 } }
                Item {}
                ChoiceBox {
                    Layout.columnSpan: 2
                    model: [{ v: "ssl", t: qsTr("TLS") }, { v: "starttls", t: "STARTTLS" }, { v: "plain", t: qsTr("None (insecure)") }]
                    textRole: "t"; valueRole: "v"
                    selected: ["ssl", "starttls", "plain"].indexOf(setup.smtpSecurity)
                    onActivated: setup.smtpSecurity = currentValue
                }
            }

            // A certificate TLS does not trust: show it and let the person
            // decide (ADR 0017).
            Rectangle {
                id: certCard
                readonly property var cert: setup.untrustedCertificate
                Layout.fillWidth: true
                visible: cert.fingerprint !== undefined
                radius: Theme.radius
                color: Theme.surface
                border.color: Theme.bad
                implicitHeight: certCol.implicitHeight + 24
                ColumnLayout {
                    id: certCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.text
                        font.weight: Font.DemiBold
                        text: certCard.cert.changed
                              ? qsTr("The certificate of %1 is not the one you trusted before.").arg(certCard.cert.host || "")
                              : certCard.cert.server === "smtp"
                                ? qsTr("The certificate of %1 (sending) is not trusted.").arg(certCard.cert.host || "")
                                : qsTr("The certificate of %1 is not trusted.").arg(certCard.cert.host || "")
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.muted
                        font.pixelSize: Theme.fontSmall + 1
                        text: certCard.cert.problems || ""
                    }
                    Label { text: qsTr("SHA-256 fingerprint"); color: Theme.muted; font.pixelSize: Theme.fontSmall }
                    TextEdit {
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.WrapAnywhere
                        color: Theme.text
                        font.family: "monospace"
                        font.pixelSize: Theme.fontSmall + 1
                        text: certCard.cert.fingerprint || ""
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.muted
                        font.pixelSize: Theme.fontSmall + 1
                        text: qsTr("Issued to %1 by %2, valid until %3.")
                              .arg(certCard.cert.subject || "?").arg(certCard.cert.issuer || "?").arg(certCard.cert.expires || "?")
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall + 1
                        text: qsTr("Trust it only if this fingerprint matches the one your server's administrator gave you. Then e3mail accepts this certificate from this server, and no other.")
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Button { text: qsTr("Trust this certificate"); enabled: !setup.busy; onClicked: setup.trustCertificate() }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                visible: setup.error.length > 0 && !certCard.visible
                radius: Theme.radius
                color: Theme.badBg
                implicitHeight: err.implicitHeight + 16
                Text {
                    id: err
                    anchors.fill: parent
                    anchors.margins: 8
                    text: setup.error
                    color: Theme.bad
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSmall + 1
                }
            }

            RowLayout {
                Layout.topMargin: 8
                PrimaryButton {
                    text: setup.busy ? qsTr("Checking…") : qsTr("Connect")
                    enabled: !setup.busy
                    Layout.preferredWidth: 140
                    onClicked: { setup.suggest(); setup.submit() }
                }
                BusyIndicator { running: setup.busy; visible: running; implicitWidth: 28; implicitHeight: 28 }
                Button { visible: view.cancellable; text: qsTr("Cancel"); flat: true; onClicked: view.cancelled() }
            }
            Button {
                visible: MailApp.devicesAvailable
                Layout.topMargin: 8
                flat: true
                text: qsTr("Copy an account from another device…")
                onClicked: joinDialog.open()
            }
        }
    }
    function openJoin() { joinDialog.open() }
    JoinDialog {
        id: joinDialog
        onJoined: view.finished()
    }
}
