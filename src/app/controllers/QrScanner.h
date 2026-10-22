// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVideoSink>

// Reads a code from the camera: QML's CaptureSession feeds `videoSink`,
// frames are decoded off the main thread one at a time (the rest are
// dropped), and `found` gives the first code seen that starts with one of
// `prefixes` (a pairing code, E3PAIR1:, unless set). Asking for the camera,
// where the platform asks, is `start()`.
class QrScanner : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVideoSink *videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    // idle | asking | ready | denied | nocamera
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QStringList prefixes MEMBER m_prefixes NOTIFY prefixesChanged)
public:
    explicit QrScanner(QObject *parent = nullptr);

    QVideoSink *videoSink() const { return m_sink; }
    void setVideoSink(QVideoSink *sink);
    QString state() const { return m_state; }

    // Whether there is a camera to ask for.
    static bool cameraPresent();
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();

Q_SIGNALS:
    void videoSinkChanged();
    void stateChanged();
    void prefixesChanged();
    void found(const QString &code);

private:
    void setState(const QString &state);
    void frame(const QVideoFrame &frame);

    QPointer<QVideoSink> m_sink;
    QString m_state = QStringLiteral("idle");
    QFutureWatcher<QString> m_watcher;
    QStringList m_prefixes{QStringLiteral("E3PAIR1:")};
};
