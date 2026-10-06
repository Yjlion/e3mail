// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>

namespace e3 {

// Where e3mail keeps its data. Resolved once, in this order:
//   1. the E3MAIL_DATA_DIR environment variable;
//   2. portable mode: a `portable.txt` file or a `data/` directory beside the
//      executable, in which case `data/` beside the executable is used, so an
//      unzipped copy leaves nothing behind on the machine (as VLC does);
//   3. the platform's application data location.
class Paths
{
public:
    static QString dataDir();
    static bool isPortable();

    // Overrides resolution, for tests and the CLI's --data-dir.
    static void setDataDir(const QString &dir);

    // First-launch disclosure acknowledgement lives beside the accounts, not in
    // any one account: it must exist before an account does and outlive them.
    static QString firstRunMarker();
};

} // namespace e3
