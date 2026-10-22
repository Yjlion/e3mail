// SPDX-License-Identifier: MPL-2.0
#include "QrDecode.h"

#include <ReadBarcode.h>

#include <QImage>

namespace qrdecode {

QString decode(const QImage &image)
{
    if (image.isNull())
        return {};
    // zxing reads luminance; a camera frame converts to it once.
    const QImage grey = image.format() == QImage::Format_Grayscale8 ? image
                                                                    : image.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(grey.constBits(), grey.width(), grey.height(), ZXing::ImageFormat::Lum,
                                int(grey.bytesPerLine()));
    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);
    options.setTryInvert(true);
    options.setMaxNumberOfSymbols(1);
    const ZXing::Barcodes found = ZXing::ReadBarcodes(view, options);
    for (const ZXing::Barcode &b : found) {
        if (b.isValid())
            return QString::fromStdString(b.text());
    }
    return {};
}

QString pairingCode(const QImage &image)
{
    return codeWithPrefix(image, {QStringLiteral("E3PAIR1:")});
}

QString codeWithPrefix(const QImage &image, const QStringList &prefixes)
{
    const QString text = decode(image);
    for (const QString &p : prefixes) {
        if (text.startsWith(p, Qt::CaseInsensitive))
            return text;
    }
    return {};
}

} // namespace qrdecode
