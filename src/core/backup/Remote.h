// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Destination.h"

#include <QJsonObject>

namespace e3::backup {

// The destinations on a network: WebDAV, S3, SFTP, Google Drive, OneDrive.
// Null for a type this build does not have.
std::unique_ptr<Destination> remoteDestination(const QJsonObject &config, const QString &secret);

} // namespace e3::backup
