// SPDX-License-Identifier: MPL-2.0
#include "UnlockGate.h"

#include "engine/AtRest.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>

bool UnlockGate::unlock(const QString &passphrase)
{
    if (passphrase.isEmpty() || !e3::AtRest::checkPassphrase(m_root, passphrase))
        return false;
    e3::AtRest::setPassphrase(passphrase);
    m_open = true;
    Q_EMIT opened();
    return true;
}

bool UnlockGate::run(const QString &root, const QString &given, const QString &grab)
{
    UnlockGate gate(root);
    if (!given.isEmpty() && gate.unlock(given))
        return true;
    const bool quitOnClose = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    {
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("Gate"), &gate);
        engine.loadFromModule("E3mail", "Unlock");
        auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (window && !grab.isEmpty()) {
            QEventLoop loop;
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            loop.exec();
            window->grabWindow().save(grab);
            window->close();
        } else if (window) {
            QEventLoop loop;
            QObject::connect(&gate, &UnlockGate::opened, &loop, &QEventLoop::quit);
            QObject::connect(window, &QWindow::visibleChanged, &loop, [&loop](bool visible) {
                if (!visible)
                    loop.quit();
            });
            loop.exec();
            window->close();
        }
    }
    QGuiApplication::setQuitOnLastWindowClosed(quitOnClose);
    return gate.isOpen();
}
