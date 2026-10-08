// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    // Phones get one pane at a time (Theme.narrow); a desktop window keeps
    // its three.
    minimumWidth: Qt.platform.os === "android" ? 0 : 360
    minimumHeight: Qt.platform.os === "android" ? 0 : 480
    visible: true
    title: MailApp.accountAddr.length ? "e3mail — " + MailApp.accountAddr : "e3mail"
    color: Theme.window

    // mail | compose | contacts | settings | setup
    property string page: "mail"
    // Narrow: the open message covers the list.
    property bool reading: false
    readonly property bool narrow: width < 720
    Binding { target: Theme; property: "narrow"; value: window.narrow }
    onPageChanged: reading = false

    function compose(kind, msgId) {
        composerView.start(kind, msgId)
        page = "compose"
    }

    // One step back in the narrow layout: Android's Back, or the bar's arrow.
    // False when there is nowhere to go back to.
    function back() {
        if (drawer.opened) drawer.close()
        else if (page === "compose") composerView.close()
        else if (reading) reading = false
        else if (page === "contacts" && contactsView.back()) return true
        else if (page !== "mail") page = "mail"
        else return false
        return true
    }
    // For screenshots (--page menu), when the window may not have its
    // narrow size yet.
    function openMenu() { drawer.open() }
    onClosing: (close) => { if (narrow && back()) close.accepted = false }

    Shortcut { sequence: StandardKey.New; onActivated: window.compose("new", 0) }
    Shortcut { sequence: "F5"; onActivated: MailApp.syncNow() }
    Shortcut { sequence: StandardKey.Find; onActivated: { page = "mail"; messageList.focusSearch() } }

    // Arabic, Hebrew and Yiddish read right to left: rows, anchors and
    // alignments mirror. Controls mirror by their locale on their own.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: MailApp.hasAccounts

        // Narrow: a title bar with the menu or a way back. The composer has its own.
        Rectangle {
            Layout.fillWidth: true
            visible: window.narrow && window.page !== "compose"
            implicitHeight: 48
            color: Theme.sidebar
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4
                IconButton {
                    readonly property bool atTop: window.page === "mail" && !window.reading
                    implicitWidth: 44; implicitHeight: 44
                    iconName: atTop ? "menu" : "back"
                    tip: atTop ? qsTr("Menu") : qsTr("Back")
                    onClicked: atTop ? drawer.open() : window.back()
                }
                Text {
                    Layout.fillWidth: true
                    text: drawerSidebar.title(window.page)
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignLeft
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Sidebar {
                visible: !window.narrow
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.sidebarWidth
                page: window.page
                onNavigate: (p) => window.page = p
                onComposeRequested: window.compose("new", 0)
            }
            Rectangle { visible: !window.narrow; Layout.fillHeight: true; width: 1; color: Theme.border }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: ["mail", "compose", "contacts", "settings", "setup"].indexOf(window.page)

                RowLayout {
                    spacing: 0
                    MessageList {
                        id: messageList
                        visible: !window.narrow || !window.reading
                        Layout.fillHeight: true
                        Layout.preferredWidth: Theme.listWidth
                        Layout.maximumWidth: window.narrow ? Number.POSITIVE_INFINITY : Theme.listWidth
                        Layout.fillWidth: window.narrow
                        onOpenDraft: (id) => window.compose("draft", id)
                        onOpened: window.reading = window.narrow
                    }
                    Rectangle { visible: !window.narrow; Layout.fillHeight: true; width: 1; color: Theme.border }
                    ReadingPane {
                        visible: !window.narrow || window.reading
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
                ContactsView { id: contactsView }
                SettingsView { onAddAccount: window.page = "setup" }
                SetupView {
                    cancellable: true
                    onFinished: window.page = "mail"
                    onCancelled: window.page = "mail"
                }
            }
        }
    }

    // Narrow: the sidebar slides in from the start edge.
    Drawer {
        id: drawer
        width: Math.min(300, window.width * 0.85)
        height: window.height
        edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.RightEdge : Qt.LeftEdge
        interactive: window.narrow && MailApp.hasAccounts
        Sidebar {
            id: drawerSidebar
            anchors.fill: parent
            page: window.page
            onNavigate: (p) => { window.page = p; window.reading = false; drawer.close() }
            onComposeRequested: { drawer.close(); window.compose("new", 0) }
        }
    }
    onNarrowChanged: if (!narrow) drawer.close()

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
