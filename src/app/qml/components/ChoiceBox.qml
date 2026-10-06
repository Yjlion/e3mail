// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls

// A ComboBox whose choice survives a language change: retranslating rebuilds
// a model of translated strings, and a plain ComboBox then falls back to its
// first entry. Bind `selected`, not currentIndex.
ComboBox {
    id: box
    property int selected: 0
    currentIndex: selected
    onModelChanged: Qt.callLater(() => box.currentIndex = Qt.binding(() => box.selected))
}
