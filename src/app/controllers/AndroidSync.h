// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

// Android's foreground service that keeps e3mail syncing while it is not in
// front (ADR 0018). Elsewhere these do nothing and `available` is false.
namespace androidsync {

bool available();
// Shows the service's notification and keeps the process running. Call
// while e3mail is in front.
void start();
void stop();
// Called on the main thread when the person presses Stop on the notification.
void onStopPressed(std::function<void()> f);

} // namespace androidsync
