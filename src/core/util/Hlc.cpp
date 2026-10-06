// SPDX-License-Identifier: MPL-2.0
#include "Hlc.h"

#include <QDateTime>

#include <algorithm>

namespace e3 {

qint64 Hlc::now()
{
    const qint64 wall = QDateTime::currentMSecsSinceEpoch() << 16;
    m_last = wall > m_last ? wall : m_last + 1;
    return m_last;
}

qint64 Hlc::observe(qint64 remote)
{
    const qint64 wall = QDateTime::currentMSecsSinceEpoch() << 16;
    m_last = std::max({wall, remote + 1, m_last + 1});
    return m_last;
}

} // namespace e3
