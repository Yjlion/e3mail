// SPDX-License-Identifier: MPL-2.0
#include "NotifierBackends.h"

#include <QGuiApplication>
#include <QHash>

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#else
#include <QSystemTrayIcon>
#endif

namespace {

#ifdef Q_OS_LINUX
const QString kService = QStringLiteral("org.freedesktop.Notifications");
const QString kPath = QStringLiteral("/org/freedesktop/Notifications");

// The freedesktop notification service: GNOME, KDE and the rest, with or
// without a system tray.
class FreedesktopBackend : public Notifier::Backend
{
    Q_OBJECT
public:
    FreedesktopBackend()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.connect(kService, kPath, kService, QStringLiteral("ActionInvoked"), this,
                    SLOT(onAction(uint, QString)));
        // Body markup, where the server has it, needs escaping.
        auto *w = new QDBusPendingCallWatcher(
            bus.asyncCall(QDBusMessage::createMethodCall(kService, kPath, kService, QStringLiteral("GetCapabilities"))),
            this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
            w->deleteLater();
            const QDBusPendingReply<QStringList> r = *w;
            m_markup = r.isValid() && r.value().contains(QStringLiteral("body-markup"));
        });
    }

    bool available() const override
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected() || !bus.interface())
            return false;
        return bus.interface()->isServiceRegistered(kService)
            || bus.interface()->activatableServiceNames().value().contains(kService);
    }

    void show(const QString &title, const QString &body, quint64 token) override
    {
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kService, QStringLiteral("Notify"));
        m << QStringLiteral("e3mail") << uint(0) << QStringLiteral("e3mail") << title
          << (m_markup ? body.toHtmlEscaped() : body)
          << QStringList{QStringLiteral("default"), tr("Open")}
          << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("e3mail")},
                         {QStringLiteral("category"), QStringLiteral("email.arrived")}}
          << int(-1);
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, token] {
            w->deleteLater();
            const QDBusPendingReply<uint> r = *w;
            if (r.isValid())
                m_tokens.insert(r.value(), token);
        });
    }

private Q_SLOTS:
    void onAction(uint id, const QString &action)
    {
        if (action == QLatin1String("default") && m_tokens.contains(id))
            Q_EMIT clicked(m_tokens.take(id));
    }

private:
    bool m_markup = false;
    QHash<uint, quint64> m_tokens; // notification id -> our token
};
#else
// Windows (a toast through the tray) and macOS (Notification Center).
class TrayBackend : public Notifier::Backend
{
    Q_OBJECT
public:
    TrayBackend()
    {
        m_icon.setIcon(QGuiApplication::windowIcon());
        m_icon.setToolTip(QStringLiteral("e3mail"));
        connect(&m_icon, &QSystemTrayIcon::messageClicked, this, [this] { Q_EMIT clicked(m_last); });
        connect(&m_icon, &QSystemTrayIcon::activated, this, [this] { Q_EMIT clicked(0); });
    }

    bool available() const override { return QSystemTrayIcon::isSystemTrayAvailable(); }

    void show(const QString &title, const QString &body, quint64 token) override
    {
        if (!m_icon.isVisible())
            m_icon.show();
        m_last = token;
        m_icon.showMessage(title, body, QGuiApplication::windowIcon(), 10000);
    }

private:
    QSystemTrayIcon m_icon;
    quint64 m_last = 0;
};
#endif

} // namespace

std::unique_ptr<Notifier::Backend> makeSystemNotifierBackend()
{
#ifdef Q_OS_LINUX
    return std::make_unique<FreedesktopBackend>();
#else
    return std::make_unique<TrayBackend>();
#endif
}

#include "NotifierBackends.moc"
