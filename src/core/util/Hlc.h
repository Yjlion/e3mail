// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QtGlobal>

namespace e3 {

// Hybrid logical clock: physical milliseconds in the high 48 bits, a logical
// counter in the low 16. Monotonic on one device even if the wall clock steps
// back, and causally ordered across devices once they have exchanged ops.
// Every op-log entry is stamped with one, so last-writer-wins is decided the
// same way on every replica.
class Hlc
{
public:
    explicit Hlc(qint64 last = 0) : m_last(last) {}

    qint64 now();
    // Advance past a timestamp received from another device.
    qint64 observe(qint64 remote);
    qint64 last() const { return m_last; }

    static qint64 physicalMs(qint64 hlc) { return hlc >> 16; }

private:
    qint64 m_last;
};

} // namespace e3
