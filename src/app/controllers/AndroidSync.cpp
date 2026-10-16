// SPDX-License-Identifier: MPL-2.0
#include "AndroidSync.h"

#include <QCoreApplication>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace androidsync {

namespace {
std::function<void()> s_stopPressed;
} // namespace

#ifdef Q_OS_ANDROID

namespace {
void stopPressed(JNIEnv *, jclass)
{
    QMetaObject::invokeMethod(qApp, [] {
        if (s_stopPressed)
            s_stopPressed();
    });
}

QJniObject context()
{
    return QNativeInterface::QAndroidApplication::context();
}

QJniObject str(const QString &s)
{
    return QJniObject::fromString(s);
}
} // namespace

bool available()
{
    return true;
}

void start()
{
    static const bool registered = [] {
        const JNINativeMethod methods[] = {{"stopPressed", "()V", reinterpret_cast<void *>(&stopPressed)}};
        QJniEnvironment env;
        return env.registerNativeMethods("org/e3mail/e3mail/E3SyncService", methods, 1);
    }();
    Q_UNUSED(registered)
    QJniObject::callStaticMethod<void>(
        "org/e3mail/e3mail/E3SyncService", "start",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
        context().object(),
        // The channel's name, as Android's settings list it.
        str(QCoreApplication::translate("AndroidSync", "Background sync")).object<jstring>(),
        str(QCoreApplication::translate("AndroidSync", "e3mail is keeping in sync")).object<jstring>(),
        str(QCoreApplication::translate("AndroidSync", "New mail and other devices' changes arrive while e3mail is in the background."))
            .object<jstring>(),
        str(QCoreApplication::translate("AndroidSync", "Stop")).object<jstring>());
}

void stop()
{
    QJniObject::callStaticMethod<void>("org/e3mail/e3mail/E3SyncService", "stop", "(Landroid/content/Context;)V",
                                       context().object());
}

#else

bool available()
{
    return false;
}

void start() {}
void stop() {}

#endif

void onStopPressed(std::function<void()> f)
{
    s_stopPressed = std::move(f);
}

} // namespace androidsync
