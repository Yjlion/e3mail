// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import E3mail

// A message's source: the original as received or sent and, for encrypted
// mail, what it decrypts to. Long runs of encoded data are shortened by
// default so the headers and text around them can be read; Copy and Save
// always take the whole of it.
Dialog {
    id: dlg
    property int messageId: 0
    property var src: ({})
    readonly property bool showDecrypted: decryptedChoice.checked && !!src.decrypted
    readonly property string fullText: showDecrypted ? (src.decrypted || "") : (src.original || "")
    readonly property var outline: (showDecrypted ? src.innerOutline : src.outline) || []

    function show(id) {
        messageId = id
        src = MailApp.messageSource(id)
        originalChoice.checked = true
        structure.checked = false
        open()
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Theme.narrow ? parent.width : Math.min(parent.width - 80, 1000)
    height: Theme.narrow ? parent.height : parent.height - 80
    modal: true
    padding: Theme.narrow ? 8 : 16
    title: qsTr("Message source")
    standardButtons: Dialog.Close

    SaveMessage { id: saver }

    // A CheckBox sized from its parts. In this Flow, right to left, a Basic
    // CheckBox with a short translated label ("מבנה", "البنية") reported an
    // implicit width without its indicator's padding, and the next button
    // covered the label. Why only there is not known (handoff, trap 37).
    component Option: CheckBox {
        id: option
        implicitWidth: leftPadding + rightPadding + implicitIndicatorWidth + spacing + Math.ceil(metrics.advanceWidth)
        TextMetrics { id: metrics; font: option.font; text: option.text }
    }

    // Copy takes the full text, not the shortened one on screen.
    TextEdit { id: clip; visible: false; textFormat: TextEdit.PlainText }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            visible: !!dlg.src.subject
            text: dlg.src.subject || ""
            color: Theme.muted
            elide: Text.ElideRight
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8
            visible: dlg.src.retained === true

            RadioButton { id: originalChoice; visible: !!dlg.src.decrypted; checked: true; text: qsTr("Original") }
            RadioButton { id: decryptedChoice; visible: !!dlg.src.decrypted; text: qsTr("Decrypted") }
            Option { id: shorten; checked: true; text: qsTr("Shorten encoded data") }
            Option { id: structure; text: qsTr("Structure") }
            Button {
                text: qsTr("Copy")
                onClicked: { clip.text = dlg.fullText; clip.selectAll(); clip.copy(); clip.text = "" }
            }
            Button {
                text: qsTr("Save…")
                onClicked: saver.save(dlg.messageId, dlg.showDecrypted)
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !!dlg.src.decryptError || dlg.src.retained === false
            wrapMode: Text.Wrap
            color: Theme.warn
            text: dlg.src.retained === false
                  ? qsTr("The original of this message is no longer kept. Change how long originals are kept in Settings.")
                  : qsTr("This message cannot be decrypted: %1").arg(dlg.src.decryptError || "")
        }

        // The MIME tree, one row per part.
        Rectangle {
            Layout.fillWidth: true
            visible: structure.checked && dlg.outline.length > 0
            implicitHeight: Math.min(parts.implicitHeight + 12, 160)
            radius: Theme.radius
            color: Theme.raised
            ScrollView {
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                Column {
                    id: parts
                    spacing: 2
                    Repeater {
                        model: dlg.outline
                        Row {
                            required property var modelData
                            spacing: 8
                            Item { width: modelData.depth * 16; height: 1 }
                            Text {
                                text: modelData.type
                                color: Theme.text
                                font.family: "monospace"
                                font.pixelSize: 12
                            }
                            Text {
                                visible: modelData.filename.length > 0
                                text: modelData.filename
                                color: Theme.accent
                                font.pixelSize: 12
                            }
                            Text {
                                text: modelData.size + (modelData.encoding.length ? " · " + modelData.encoding : "")
                                color: Theme.muted
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: dlg.src.retained === true
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.border

            // Source is ASCII and always reads left to right.
            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true
                TextArea {
                    id: area
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Theme.narrow ? TextEdit.WrapAnywhere : TextEdit.NoWrap
                    textFormat: TextEdit.PlainText
                    horizontalAlignment: TextEdit.AlignLeft
                    font.family: "monospace"
                    font.pixelSize: 12
                    color: Theme.text
                    selectionColor: Theme.selection
                    selectedTextColor: Theme.text
                    background: null
                    text: shorten.checked ? MailApp.shortenSource(dlg.fullText) : dlg.fullText
                }
            }
            SourceHighlighter {
                document: area.textDocument
                nameColor: Theme.accent
                mutedColor: Theme.muted
                armorColor: Theme.good
            }
        }
    }
}
