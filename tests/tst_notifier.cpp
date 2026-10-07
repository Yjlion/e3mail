// SPDX-License-Identifier: MPL-2.0
#include "Notifier.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

// Records what would have been shown.
class Recorder : public Notifier::Backend
{
public:
    struct Shown
    {
        QString title, body;
        quint64 token;
    };
    QList<Shown> shown;
    bool isAvailable = true;

    bool available() const override { return isAvailable; }
    void show(const QString &title, const QString &body, quint64 token) override { shown.append({title, body, token}); }
    void click(quint64 token) { Q_EMIT clicked(token); }
};

} // namespace

class TestNotifier : public QObject
{
    Q_OBJECT

    Recorder *m_rec = nullptr;
    std::unique_ptr<Notifier> m_n;
    bool m_active = false;

private Q_SLOTS:
    void init()
    {
        auto rec = std::make_unique<Recorder>();
        m_rec = rec.get();
        m_n = std::make_unique<Notifier>(std::move(rec));
        m_n->setGatherMs(50);
        m_active = false;
        m_n->setWindowActive([this] { return m_active; });
    }

    void senderOnlyByDefault()
    {
        QCOMPARE(m_n->mode(), Notifier::Mode::Sender);
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira Dorn"), QStringLiteral("Salary review"));
        QTRY_COMPARE(m_rec->shown.size(), 1);
        QCOMPARE(m_rec->shown[0].title, QStringLiteral("Mira Dorn"));
        QVERIFY(!m_rec->shown[0].body.contains(QLatin1String("Salary")));
    }

    void fullShowsTheSubject()
    {
        m_n->setMode(Notifier::Mode::Full);
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira Dorn"), QStringLiteral("Salary review"));
        QTRY_COMPARE(m_rec->shown.size(), 1);
        QCOMPARE(m_rec->shown[0].body, QStringLiteral("Salary review"));
    }

    void offShowsNothing()
    {
        m_n->setMode(Notifier::Mode::Off);
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira"), QStringLiteral("s"));
        QTest::qWait(150);
        QVERIFY(m_rec->shown.isEmpty());
    }

    void aBurstIsOneNotification()
    {
        m_n->setMode(Notifier::Mode::Full);
        for (int i = 0; i < 6; ++i)
            m_n->arrived(1, QStringLiteral("ada@x.test"), i, QStringLiteral("Sender %1").arg(i % 5), QStringLiteral("s"));
        QTRY_COMPARE(m_rec->shown.size(), 1);
        QTest::qWait(100);
        QCOMPARE(m_rec->shown.size(), 1);
        QVERIFY2(m_rec->shown[0].title.contains(QLatin1String("6")), qPrintable(m_rec->shown[0].title));
        // Five senders: three named, the rest counted. No subjects in a summary.
        QVERIFY2(m_rec->shown[0].body.contains(QLatin1String("Sender 0, Sender 1, Sender 2")),
                 qPrintable(m_rec->shown[0].body));
        QVERIFY(!m_rec->shown[0].body.contains(QLatin1String("Sender 3")));
    }

    void nothingWhileTheWindowIsActive()
    {
        m_active = true;
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira"), QStringLiteral("s"));
        QTest::qWait(150);
        QVERIFY(m_rec->shown.isEmpty());
    }

    void unavailableShowsNothing()
    {
        m_rec->isAvailable = false;
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira"), QStringLiteral("s"));
        QTest::qWait(150);
        QVERIFY(m_rec->shown.isEmpty());
    }

    void accountsAreSeparateAndNamed()
    {
        m_n->setAccountCount(2);
        m_n->arrived(1, QStringLiteral("ada@x.test"), 10, QStringLiteral("Mira"), QStringLiteral("s"));
        m_n->arrived(2, QStringLiteral("work@y.test"), 20, QStringLiteral("Tomas"), QStringLiteral("s"));
        QTRY_COMPARE(m_rec->shown.size(), 2);
        QStringList bodies{m_rec->shown[0].body, m_rec->shown[1].body};
        bodies.sort();
        QVERIFY(bodies[0].endsWith(QLatin1String("\nada@x.test")));
        QVERIFY(bodies[1].endsWith(QLatin1String("\nwork@y.test")));
    }

    void clickingOpensTheMessage()
    {
        QSignalSpy spy(m_n.get(), &Notifier::activated);
        m_n->arrived(7, QStringLiteral("ada@x.test"), 42, QStringLiteral("Mira"), QStringLiteral("s"));
        QTRY_COMPARE(m_rec->shown.size(), 1);
        m_rec->click(m_rec->shown[0].token);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy[0][0].toInt(), 7);
        QCOMPARE(spy[0][1].toLongLong(), 42);
        // The tray icon itself: no message, just the window.
        m_rec->click(0);
        QCOMPARE(spy[1][0].toInt(), -1);
    }

    void modeNames()
    {
        for (auto m : {Notifier::Mode::Off, Notifier::Mode::Sender, Notifier::Mode::Full})
            QCOMPARE(Notifier::modeFromString(Notifier::modeToString(m)), m);
        QCOMPARE(Notifier::modeFromString(QString()), Notifier::Mode::Sender);
    }
};

QTEST_GUILESS_MAIN(TestNotifier)
#include "tst_notifier.moc"
