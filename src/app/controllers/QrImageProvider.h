// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QQuickImageProvider>

// image://qr/<percent-encoded text>: the text as a QR code, black on white
// with the quiet zone the standard asks for, so a camera reads it in either
// theme. Used for the pairing code.
class QrImageProvider : public QQuickImageProvider
{
public:
    QrImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
