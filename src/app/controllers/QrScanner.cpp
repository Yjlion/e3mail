// SPDX-License-Identifier: MPL-2.0
#include "QrScanner.h"
#include "QrDecode.h"

#include <QCameraDevice>
#include <QCoreApplication>
#include <QImage>
#include <QMediaDevices>
#include <QPermissions>
#include <QVideoFrame>
#include <QtConcurrentRun>

namespace {
// Larger frames take longer to read and find nothing more in a code that
// fills the guide.
constexpr int kMaxSide = 1280;
} // namespace

QrScanner::QrScanner(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<QString>::finished, this, [this] {
        const QString text = m_watcher.result();
        if (m_state == QLatin1String("ready") && !text.isEmpty()) {
            stop();
            Q_EMIT found(text);
        }
    });
}

bool QrScanner::cameraPresent()
{
    return !QMediaDevices::videoInputs().isEmpty();
}

void QrScanner::setVideoSink(QVideoSink *sink)
{
    if (sink == m_sink)
        return;
    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);
    m_sink = sink;
    if (m_sink)
        connect(m_sink, &QVideoSink::videoFrameChanged, this, &QrScanner::frame);
    Q_EMIT videoSinkChanged();
}

void QrScanner::setState(const QString &state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged();
}

void QrScanner::start()
{
    if (!cameraPresent()) {
        setState(QStringLiteral("nocamera"));
        return;
    }
    QCameraPermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        setState(QStringLiteral("ready"));
        return;
    case Qt::PermissionStatus::Denied:
        // Refused before: only the system's settings can change it now.
        setState(QStringLiteral("denied"));
        return;
    case Qt::PermissionStatus::Undetermined:
        setState(QStringLiteral("asking"));
        qApp->requestPermission(permission, this, [this](const QPermission &p) {
            if (m_state != QLatin1String("asking"))
                return; // cancelled meanwhile
            setState(p.status() == Qt::PermissionStatus::Granted ? QStringLiteral("ready") : QStringLiteral("denied"));
        });
        return;
    }
}

void QrScanner::stop()
{
    setState(QStringLiteral("idle"));
}

void QrScanner::frame(const QVideoFrame &frame)
{
    if (m_state != QLatin1String("ready") || m_watcher.isRunning() || !frame.isValid())
        return;
    QImage image = frame.toImage();
    if (image.isNull())
        return;
    if (qMax(image.width(), image.height()) > kMaxSide)
        image = image.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio, Qt::FastTransformation);
    m_watcher.setFuture(QtConcurrent::run([image] { return qrdecode::pairingCode(image); }));
}
