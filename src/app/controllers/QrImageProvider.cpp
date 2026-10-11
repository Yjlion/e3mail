// SPDX-License-Identifier: MPL-2.0
#include "QrImageProvider.h"

#include "qrcodegen.hpp"

#include <QPainter>
#include <QUrl>

QImage QrImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QByteArray text = QUrl::fromPercentEncoding(id.toUtf8()).toUtf8();
    using qrcodegen::QrCode;
    const QrCode qr = QrCode::encodeText(text.constData(), QrCode::Ecc::LOW);
    constexpr int kQuiet = 4;
    const int modules = qr.getSize() + 2 * kQuiet;
    const int want = requestedSize.isValid() ? qMin(requestedSize.width(), requestedSize.height()) : modules * 6;
    const int scale = qMax(1, want / modules);
    QImage img(modules * scale, modules * scale, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (qr.getModule(x, y))
                p.fillRect((x + kQuiet) * scale, (y + kQuiet) * scale, scale, scale, Qt::black);
        }
    }
    p.end();
    if (size)
        *size = img.size();
    return img;
}
