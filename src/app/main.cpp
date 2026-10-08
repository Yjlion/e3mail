// SPDX-License-Identifier: MPL-2.0
#include "NetworkBarrier.h"
#include "controllers/Languages.h"
#include "controllers/MailApp.h"
#include "engine/AccountManager.h"
#include "util/Paths.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#ifndef Q_OS_LINUX // which Android defines too
#include <QApplication> // the system tray, for notifications
#endif
#include <QIcon>
#include <QQmlApplicationEngine>

#include "InlineImages.h"
#include <QWindow>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    QGuiApplication app(argc, argv);
#else
    QApplication app(argc, argv);
#endif
    QGuiApplication::setApplicationName(QStringLiteral("e3mail"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("e3mail"));
    QGuiApplication::setOrganizationName(QStringLiteral("e3mail"));
    QGuiApplication::setApplicationVersion(QStringLiteral(E3MAIL_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("e3mail"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/E3mail/qml/icons/e3mail.svg")));

    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("An end-to-end encrypted email client."));
    p.addHelpOption();
    p.addVersionOption();
    const QCommandLineOption dataDir(QStringLiteral("data-dir"), QStringLiteral("Use this data directory."),
                                     QStringLiteral("dir"));
    const QCommandLineOption smoke(QStringLiteral("smoke-test"), QStringLiteral("Load the UI and exit."));
    const QCommandLineOption grab(QStringLiteral("grab"), QStringLiteral("Save a screenshot of the window and exit."),
                                  QStringLiteral("png"));
    const QCommandLineOption offline(QStringLiteral("offline"), QStringLiteral("Do not connect to mail servers."));
    const QCommandLineOption page(QStringLiteral("page"), QStringLiteral("Start on this page (for screenshots)."),
                                  QStringLiteral("mail|compose|reply|contacts|settings|menu"));
    const QCommandLineOption open(QStringLiteral("open"), QStringLiteral("Open the newest message in a tag (for screenshots)."),
                                  QStringLiteral("tag"));
    const QCommandLineOption lang(QStringLiteral("lang"),
                                  QStringLiteral("Show the interface in this language, without saving it."),
                                  QStringLiteral("code"));
    const QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Open the window at this size (for screenshots)."),
                                  QStringLiteral("WxH"));
    p.addOptions({dataDir, smoke, grab, offline, page, open, lang, size});
    p.process(app);
    if (p.isSet(dataDir))
        e3::Paths::setDataDir(p.value(dataDir));
    Languages::install(p.isSet(lang) ? p.value(lang) : Languages::saved());

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    e3::AccountManager accounts(e3::Paths::dataDir());
    MailApp mailApp(&accounts);
    MailApp::setInstance(&mailApp);
    if (!p.isSet(offline) && !p.isSet(smoke) && !p.isSet(grab))
        accounts.startAll();

    QQmlApplicationEngine engine;
    NetworkBarrierFactory barrier;
    engine.setNetworkAccessManagerFactory(&barrier);
    QObject::connect(&mailApp, &MailApp::languageChanged, &engine, &QQmlEngine::retranslate);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.addImageProvider(QStringLiteral("cid"), new InlineImages); // the engine owns it
    engine.loadFromModule("E3mail", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    QObject *root = engine.rootObjects().first();
    if (p.isSet(size)) {
        root->setProperty("width", p.value(size).section(u'x', 0, 0).toInt());
        root->setProperty("height", p.value(size).section(u'x', 1, 1).toInt());
    }
#ifdef Q_OS_ANDROID
    // Android drops the IDLE connections of an app in the background; catch up
    // when it comes back. There is no background service (ADR 0016).
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &mailApp, [&mailApp](Qt::ApplicationState s) {
        if (s == Qt::ApplicationActive)
            mailApp.syncNow();
    });
#endif
    // A clicked notification brings the window to the front.
    if (auto *w = qobject_cast<QWindow *>(root)) {
        QObject::connect(&mailApp, &MailApp::raiseRequested, w, [w] {
            w->show();
            w->raise();
            w->requestActivate();
        });
    }
    if (p.isSet(open)) {
        // "inbox" or "inbox:2" (the third newest)
        mailApp.selectTag(p.value(open).section(u':', 0, 0));
        const int row = p.value(open).section(u':', 1, 1).toInt();
        if (mailApp.messages()->rowCount() > row)
            mailApp.selectMessage(mailApp.messages()->idAt(row));
    }
    if (p.isSet(page)) {
        if (p.value(page) == QLatin1String("compose"))
            QMetaObject::invokeMethod(root, "compose", Q_ARG(QVariant, QStringLiteral("new")), Q_ARG(QVariant, 0));
        else if (p.value(page) == QLatin1String("reply")) // to the message --open selected
            QMetaObject::invokeMethod(root, "compose", Q_ARG(QVariant, QStringLiteral("reply")),
                                      Q_ARG(QVariant, mailApp.selectedMessageId()));
        else if (p.value(page) == QLatin1String("menu")) // the narrow layout's drawer
            QMetaObject::invokeMethod(root, "openMenu");
        else if (p.value(page) == QLatin1String("source")) // of the message --open selected
            QMetaObject::invokeMethod(root, "showSource");
        else
            root->setProperty("page", p.value(page));
    }
    // In the narrow layout, the opened message covers the list.
    if (p.isSet(open) && (!p.isSet(page) || p.value(page) == QLatin1String("source")))
        root->setProperty("reading", true);

    if (p.isSet(smoke) || p.isSet(grab)) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString out = p.value(grab);
        QTimer::singleShot(1500, &app, [window, out] {
            int code = 0;
            if (!out.isEmpty() && window)
                code = window->grabWindow().save(out) ? 0 : 2;
            QCoreApplication::exit(code);
        });
    }
    const int rc = app.exec();
    accounts.stopAll();
    return rc;
}
