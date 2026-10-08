// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QUrl>

#include <optional>

// Files the person picked in a file dialog. On the desktop that is a local
// file, written atomically. On Android the dialog returns a content:// URI,
// which QFile opens through the storage framework but QSaveFile cannot.
namespace FileIo {

bool write(const QUrl &url, const QByteArray &data);
std::optional<QByteArray> read(const QUrl &url);
// The name to show for a picked file.
QString displayName(const QUrl &url);

} // namespace FileIo
