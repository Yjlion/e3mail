// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QImage>
#include <QQuickImageProvider>

// Images a message carries itself (multipart/related, referenced as cid:),
// shown in its HTML as image://cid/<key>. Nothing here touches the network:
// the bytes are the message's own stored parts, decoded when the message is
// shown (ADR 0009, amendment of 2026-10-07).
class InlineImages : public QQuickImageProvider
{
public:
    static constexpr int kMaxWidth = 600;
    static constexpr qint64 kMaxBytes = 5 * 1024 * 1024;

    InlineImages() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // Keeps a decoded image and returns its image:// URL. Images of the most
    // recently shown messages are kept; older ones are let go.
    static QString put(const QString &message, int index, const QImage &image);
    // Decodes a part for inline display: PNG, JPEG, GIF or WebP only, at most
    // kMaxBytes, scaled down to kMaxWidth. Null when it is none of those.
    static QImage decode(const QByteArray &data, const QString &mimeType);
};
