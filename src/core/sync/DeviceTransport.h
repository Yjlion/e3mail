// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace e3::sync {

// Moves bytes between this device and the other devices of the same account.
// Non-blocking: everything that arrives arrives as a signal. Devices are named
// by their device id. Iroh sits behind this (ADR 0010); tests use a loopback.
// A transport only moves bytes: what they mean is SyncSession's business.
class DeviceTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    // In order, reliably, while the device stays connected.
    virtual void send(const QString &device, const QByteArray &message) = 0;
    // Answered by blobReceived or blobMissing.
    virtual void requestBlob(const QString &device, const QString &name) = 0;
    // Answers a blobRequested; empty data means "not here".
    virtual void provideBlob(const QString &device, const QString &name, const QByteArray &data) = 0;

Q_SIGNALS:
    void connected(const QString &device);
    void disconnected(const QString &device);
    void received(const QString &device, const QByteArray &message);
    void blobRequested(const QString &device, const QString &name);
    void blobReceived(const QString &device, const QString &name, const QByteArray &data);
    void blobMissing(const QString &device, const QString &name);
};

} // namespace e3::sync
