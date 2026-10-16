// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

class QImage;

namespace qrdecode {

// The text of the first QR code in the image, or empty if there is none.
// Rotated, mirrored-colour and small codes are tried too.
QString decode(const QImage &image);
// The same, but only a pairing code (E3PAIR1:…); anything else is empty.
QString pairingCode(const QImage &image);

} // namespace qrdecode
