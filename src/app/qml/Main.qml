// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    title: MailApp.accountAddr.length ? "e3mail — " + MailApp.accountAddr : "e3mail"
    color: Theme.window

    // mail | compose | contacts | settings | setup
    property string page: "mail"

    function compose(kind, msgId) {
        composerView.start(kind, msgId)
        page = "compose"
    }

    Shortcut { sequence: StandardKey.New; onActivated: window.compose("new", 0) }
    Shortcut { sequence: "F5"; onActivated: MailApp.syncNow() }
    Shortcut { sequence: StandardKey.Find; onActivated: { page = "mail"; messageList.focusSearch() } }

    // Arabic, Hebrew and Yiddish read right to left: rows, anchors and
    // alignments mirror. Controls mirror by their locale on their own.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: MailApp.hasAccounts

        Sidebar {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebarWidth
            page: window.page
            onNavigate: (p) => window.page = p
            onComposeRequested: window.compose("new", 0)
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: Theme.border }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: ["mail", "compose", "contacts", "settings", "setup"].indexOf(window.page)

            RowLayout {
                spacing: 0
                MessageList {
                    id: messageList
                    Layout.fillHeight: true
                    Layout.preferredWidth: Theme.listWidth
                    Layout.maximumWidth: Theme.listWidth
                    Layout.fillWidth: false
                    onOpenDraft: (id) => window.compose("draft", id)
                }
                Rectangle { Layout.fillHeight: true; width: 1; color: Theme.border }
                ReadingPane {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    onReply: (id, all) => window.compose(all ? "replyAll" : "reply", id)
                    onForward: (id) => window.compose("forward", id)
                    onEditDraft: (id) => window.compose("draft", id)
                }
            }
            ComposerView {
                id: composerView
                onClosed: window.page = "mail"
            }
            ContactsView {}
            SettingsView { onAddAccount: window.page = "setup" }
            SetupView {
                cancellable: true
                onFinished: window.page = "mail"
                onCancelled: window.page = "mail"
            }
        }
    }

    // No account yet: the setup form is the whole window.
    SetupView {
        anchors.fill: parent
        visible: !MailApp.hasAccounts
        cancellable: false
    }

    FirstRunDialog {
        id: firstRun
        anchors.centerIn: parent
    }
    Component.onCompleted: if (!MailApp.firstRunAcknowledged) firstRun.open()

    // Transient notices.
    Rectangle {
        id: toast
        property string message
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        radius: Theme.radius
        color: Theme.text
        opacity: message.length ? 0.92 : 0
        visible: opacity > 0
        width: toastText.implicitWidth + 32
        height: toastText.implicitHeight + 18
        Behavior on opacity { NumberAnimation { duration: 150 } }
        Text {
            id: toastText
            anchors.centerIn: parent
            text: toast.message
            color: Theme.window
            font.pixelSize: Theme.fontBody
        }
        Timer { id: toastTimer; interval: 3500; onTriggered: toast.message = "" }
    }
    Connections {
        target: MailApp
        function onNotify(message) { toast.message = message; toastTimer.restart() }
    }
}
