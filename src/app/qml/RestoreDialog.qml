// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// Setup's "Restore from a backup" (ADR 0020).
Dialog {
    id: dlg
    signal restored()
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    width: Math.min(480, parent.width - (Theme.narrow ? 16 : 96))
    height: Math.min(implicitHeight, parent.height - 32)
    title: qsTr("Restore from a backup")
    standardButtons: Dialog.Cancel

    BackupController {
        id: restorer
        onRestored: { dlg.close(); dlg.restored() }
    }

    Flickable {
        anchors.fill: parent
        implicitHeight: column.implicitHeight
        contentHeight: column.implicitHeight
        clip: true
        ColumnLayout {
            id: column
            width: parent.width
            spacing: 8
            BackupForm { id: form; Layout.fillWidth: true; controller: restorer; purpose: "restore" }
            ProgressBar { Layout.fillWidth: true; visible: restorer.busy; from: 0; to: 100; value: restorer.percent }
            PrimaryButton {
                text: restorer.busy ? qsTr("Restoring…") : qsTr("Restore")
                enabled: !restorer.busy && form.ready()
                onClicked: restorer.restore(form.destination(), form.secret(), form.passphrase())
            }
        }
    }
}
