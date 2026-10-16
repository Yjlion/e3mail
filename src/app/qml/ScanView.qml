// SPDX-License-Identifier: MPL-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import E3mail

// The camera, reading the pairing code that the other device shows
// (JoinDialog). Loaded only in builds with a camera (E3MAIL_CAMERA).
ColumnLayout {
    id: view
    signal found(string code)
    signal cancelled()
    spacing: 8

    QrScanner {
        id: scanner
        videoSink: output.videoSink
        onFound: (code) => view.found(code)
    }
    MediaDevices { id: devices }
    CaptureSession {
        camera: Camera {
            // The camera on the back, where a phone has one.
            cameraDevice: {
                const inputs = devices.videoInputs
                for (let i = 0; i < inputs.length; ++i)
                    if (inputs[i].position === CameraDevice.BackFace)
                        return inputs[i]
                return devices.defaultVideoInput
            }
            focusMode: Camera.FocusModeAuto
            active: scanner.state === "ready"
        }
        videoOutput: output
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(width, 360)
        color: "black"
        radius: Theme.radius
        clip: true
        VideoOutput {
            id: output
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectCrop
        }
        // Where the code should be.
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height) * 0.7
            height: width
            color: "transparent"
            border.color: "white"
            border.width: 2
            radius: 8
            visible: scanner.state === "ready"
        }
        Label {
            anchors.centerIn: parent
            width: parent.width - 32
            visible: scanner.state !== "ready"
            color: "white"
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            text: scanner.state === "nocamera" ? qsTr("This device has no camera. Type the code instead.")
                : scanner.state === "denied" ? qsTr("e3mail may not use the camera. Allow it in the system settings, or type the code instead.")
                : qsTr("Asking for the camera…")
        }
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        visible: scanner.state === "ready"
        text: qsTr("Point the camera at the QR code on the other device.")
    }
    Button {
        flat: true
        text: qsTr("Type the code instead")
        onClicked: { scanner.stop(); view.cancelled() }
    }
    Component.onCompleted: scanner.start()
}
