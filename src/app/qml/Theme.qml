// SPDX-License-Identifier: MPL-2.0
pragma Singleton
import QtQuick

// Colours and metrics in one place, following the system's light or dark
// preference.
QtObject {
    readonly property bool dark: Qt.styleHints.colorScheme === Qt.ColorScheme.Dark

    readonly property color window: dark ? "#16181d" : "#ffffff"
    readonly property color sidebar: dark ? "#1d2027" : "#f5f6f8"
    readonly property color surface: dark ? "#1f232b" : "#ffffff"
    readonly property color raised: dark ? "#272b35" : "#f0f2f5"
    readonly property color border: dark ? "#2f3440" : "#e3e6eb"
    readonly property color text: dark ? "#e7e9ee" : "#1b1e24"
    readonly property color muted: dark ? "#9aa1ad" : "#636b78"
    readonly property color accent: dark ? "#6d9bff" : "#2457d6"
    readonly property color accentText: "#ffffff"
    readonly property color selection: dark ? "#2a3550" : "#dde6fb"
    readonly property color hover: dark ? "#252a33" : "#eef1f5"

    readonly property color good: dark ? "#4ccf8c" : "#1a7f4b"
    readonly property color goodBg: dark ? "#163527" : "#e6f6ec"
    readonly property color info: dark ? "#7ea6ff" : "#2457d6"
    readonly property color infoBg: dark ? "#1b2a4a" : "#e7eefc"
    readonly property color warn: dark ? "#f2b84b" : "#9a5b00"
    readonly property color warnBg: dark ? "#3a2d12" : "#fff3dc"
    readonly property color bad: dark ? "#ff7b72" : "#b42318"
    readonly property color badBg: dark ? "#3d1b1a" : "#fdecea"

    readonly property int radius: 6
    readonly property int pad: 12
    readonly property int sidebarWidth: 220
    readonly property int listWidth: 360
    readonly property real fontSmall: 11
    readonly property real fontBody: 13
    readonly property real fontTitle: 17

    function icon(name) { return Qt.resolvedUrl("icons/" + name + ".svg") }
}
