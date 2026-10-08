// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Types.h"

#include <QByteArray>

namespace e3::mail {

// The address book as a spreadsheet: RFC 4180, CRLF, UTF-8 with a byte order
// mark (without it, Excel reads UTF-8 as the system's code page). One row per
// contact: Name, Email, Organization, Title, Mobile/Work/Home/Other Phone,
// Birthday, Notes. Several numbers of one kind share a cell, " ; " apart.
//
// Names come from strangers' mail, so a cell a spreadsheet would run as a
// formula (starting with = + - @, a tab or a carriage return) is prefixed
// with an apostrophe, unless it holds only a phone number's characters.
class ContactsCsv
{
public:
    static QByteArray emit(const QList<ContactInfo> &contacts);
};

} // namespace e3::mail
