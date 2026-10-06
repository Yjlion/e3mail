// SPDX-License-Identifier: MPL-2.0
#include "NetworkBarrier.h"
#include "controllers/MailApp.h"
#include "engine/AccountManager.h"
#include "util/Paths.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
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
                                  QStringLiteral("mail|compose|contacts|settings"));
    const QCommandLineOption open(QStringLiteral("open"), QStringLiteral("Open the newest message in a tag (for screenshots)."),
                                  QStringLiteral("tag"));
    p.addOptions({dataDir, smoke, grab, offline, page, open});
    p.process(app);
    if (p.isSet(dataDir))
        e3::Paths::setDataDir(p.value(dataDir));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    e3::AccountManager accounts(e3::Paths::dataDir());
    MailApp mailApp(&accounts);
    MailApp::setInstance(&mailApp);
    if (!p.isSet(offline) && !p.isSet(smoke) && !p.isSet(grab))
        accounts.startAll();

    QQmlApplicationEngine engine;
    NetworkBarrierFactory barrier;
    engine.setNetworkAccessManagerFactory(&barrier);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("E3mail", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    QObject *root = engine.rootObjects().first();
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
        else
            root->setProperty("page", p.value(page));
    }

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
