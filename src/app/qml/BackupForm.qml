// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Where backups go, and how to open them (ADR 0020): used by Settings to set
// backups up and by setup to restore one.
ColumnLayout {
    id: form
    required property BackupController controller
    // "setup" or "restore"
    property string purpose: "setup"
    readonly property string type: typeBox.currentValue || "folder"
    readonly property bool cloud: type === "gdrive" || type === "onedrive"
    property string recoveryCode: ""
    spacing: 6

    function destination() {
        if (type === "folder") return { type: type, path: pathField.text.trim() }
        if (type === "webdav") return { type: type, url: urlField.text.trim(), user: userField.text.trim() }
        if (type === "s3") return { type: type, endpoint: urlField.text.trim(), region: regionField.text.trim(),
                                   bucket: bucketField.text.trim(), prefix: prefixField.text.trim(),
                                   accessKey: userField.text.trim(), pathStyle: true }
        if (type === "sftp") return { type: type, host: hostField.text.trim(), port: parseInt(portField.text) || 22,
                                     user: userField.text.trim(), path: pathField.text.trim() }
        return { type: type }
    }
    function secret() { return secretField.text }
    function passphrase() { return form.recoveryCode.length ? form.recoveryCode : passField.text }
    function ready() {
        if (form.cloud && !controller.cloudSignedIn) return false
        if (form.passphrase().length < 12) return false
        if (form.purpose === "setup" && !form.recoveryCode.length && passField.text !== againField.text) return false
        return true
    }

    Label { text: qsTr("Where"); color: Theme.muted }
    ChoiceBox {
        id: typeBox
        Layout.fillWidth: true
        textRole: "t"; valueRole: "v"
        model: controller.types.map(v => ({ v: v, t: v === "folder" ? qsTr("A folder (a drive, or one another program syncs)")
                                                 : v === "webdav" ? qsTr("WebDAV (Nextcloud and others)")
                                                 : v === "s3" ? qsTr("S3 and compatible (Backblaze, Wasabi, R2…)")
                                                 : v === "sftp" ? qsTr("SFTP (an SSH server)")
                                                 : v === "gdrive" ? qsTr("Google Drive") : qsTr("OneDrive") }))
    }
    Label { visible: hostField.visible; text: qsTr("Server"); color: Theme.muted }
    RowLayout {
        Layout.fillWidth: true
        visible: form.type === "sftp"
        Field { id: hostField; Layout.fillWidth: true; placeholderText: "backup.example.org" }
        Field { id: portField; Layout.preferredWidth: 70; text: "22"; inputMethodHints: Qt.ImhDigitsOnly }
    }
    Label { visible: urlField.visible; text: form.type === "s3" ? qsTr("Endpoint") : qsTr("Address"); color: Theme.muted }
    Field {
        id: urlField
        Layout.fillWidth: true
        visible: form.type === "webdav" || form.type === "s3"
        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
        placeholderText: form.type === "s3" ? "https://s3.eu-central-1.amazonaws.com" : "https://cloud.example.org/remote.php/dav/files/me/e3mail/"
    }
    GridLayout {
        Layout.fillWidth: true
        visible: form.type === "s3"
        columns: 2
        Label { text: qsTr("Bucket"); color: Theme.muted }
        Label { text: qsTr("Region"); color: Theme.muted }
        Field { id: bucketField; Layout.fillWidth: true }
        Field { id: regionField; Layout.fillWidth: true; text: "us-east-1" }
        Label { text: qsTr("Folder in the bucket"); color: Theme.muted; Layout.columnSpan: 2 }
        Field { id: prefixField; Layout.fillWidth: true; Layout.columnSpan: 2; text: "e3mail" }
    }
    Label { visible: pathField.visible; text: form.type === "folder" ? qsTr("Folder") : qsTr("Folder on the server"); color: Theme.muted }
    Field { id: pathField; Layout.fillWidth: true; visible: form.type === "folder" || form.type === "sftp" }
    Label {
        visible: userField.visible
        text: form.type === "s3" ? qsTr("Access key") : qsTr("User name")
        color: Theme.muted
    }
    Field { id: userField; Layout.fillWidth: true; visible: form.type !== "folder" && !form.cloud }
    Label {
        visible: secretField.visible
        text: form.type === "s3" ? qsTr("Secret key") : qsTr("Password")
        color: Theme.muted
    }
    Field { id: secretField; Layout.fillWidth: true; visible: form.type !== "folder" && !form.cloud; echoMode: TextInput.Password }
    Button {
        visible: form.cloud
        enabled: !controller.busy
        text: controller.cloudSignedIn ? qsTr("Signed in") : form.type === "gdrive" ? qsTr("Sign in to Google Drive") : qsTr("Sign in to OneDrive")
        onClicked: controller.signInToCloud(form.type)
    }

    Label { text: form.purpose === "setup" ? qsTr("Passphrase for the backups") : qsTr("Passphrase or recovery code"); color: Theme.muted; Layout.topMargin: 6 }
    Field { id: passField; Layout.fillWidth: true; echoMode: TextInput.Password; visible: !form.recoveryCode.length }
    Field {
        id: againField
        Layout.fillWidth: true
        visible: form.purpose === "setup" && !form.recoveryCode.length
        echoMode: TextInput.Password
        placeholderText: qsTr("Again")
    }
    Rectangle {
        Layout.fillWidth: true
        visible: form.recoveryCode.length > 0
        implicitHeight: codeText.implicitHeight + 16
        radius: Theme.radius
        color: Theme.surface
        border.color: Theme.border
        Text {
            id: codeText
            anchors.fill: parent
            anchors.margins: 8
            text: form.recoveryCode
            color: Theme.text
            font.family: "monospace"
            font.pixelSize: 16
            horizontalAlignment: Text.AlignHCenter
        }
    }
    Button {
        visible: form.purpose === "setup"
        flat: true
        text: form.recoveryCode.length ? qsTr("Use a passphrase instead") : qsTr("Make a recovery code instead")
        onClicked: form.recoveryCode = form.recoveryCode.length ? "" : controller.recoveryCode()
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.muted
        font.pixelSize: Theme.fontSmall + 1
        text: form.purpose === "setup"
              ? qsTr("Everything is encrypted on this device with it before it leaves. Without it the backups cannot be opened, by anyone: write it down and keep it apart from this device.")
              : qsTr("A new device of the account is made from the newest backup there. It does not sync with your other devices until you pair it.")
    }

    // A certificate or SSH host key to check before going on (ADR 0017).
    ColumnLayout {
        Layout.fillWidth: true
        visible: controller.confirm.kind !== undefined
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.bad
            text: controller.confirm.kind === "hostKey"
                  ? (controller.confirm.changed ? qsTr("The server's SSH host key has changed. Trust the new one only if the server's administrator says it changed:")
                                                : qsTr("The server's SSH host key is not known yet. Trust it only if it matches what the server's administrator gives you:"))
                  : qsTr("The server's certificate is not trusted. Trust it only if its fingerprint matches what the server's administrator gives you:")
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            color: Theme.text
            font.family: "monospace"
            text: controller.confirm.fingerprint || ""
        }
        Button { text: qsTr("Trust it"); onClicked: controller.trust() }
    }
    Text {
        Layout.fillWidth: true
        visible: controller.error.length > 0
        wrapMode: Text.Wrap
        color: Theme.bad
        text: controller.error
    }
}
