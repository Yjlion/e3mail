// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Notifier.h"

// The platform's own: the freedesktop notification service on Linux, the
// system tray on Windows and macOS.
std::unique_ptr<Notifier::Backend> makeSystemNotifierBackend();
