// SPDX-License-Identifier: MPL-2.0
#include "QrDecode.h"
#include "QrImageProvider.h"
#include "engine/Pairing.h"
#include "mail/SecureJoin.h"

#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTransform>
#include <QUrl>
#include <QtTest>

namespace {

// A code as long as a real one gets: n0's relay and six addresses, IPv6
// among them.
QString longCode()
{
    e3::pairing::Code c;
    c.addr = QStringLiteral(
        R"({"id":"5f1c0e7a9b3d42e18c6a0f7d2b4e9a1c3d5f7b9e0a2c4e6f8a1b3d5f7e9c0a2b","addrs":[)"
        R"({"Relay":"https://euw1-1.relay.n0.iroh-canary.iroh.link./"},{"Ip":"192.168.178.23:41234"},)"
        R"({"Ip":"10.0.2.15:41234"},{"Ip":"172.17.0.1:41234"},{"Ip":"[2a02:8109:b6c0:1d00:4c3e:91ff:fe2a:7b10]:41234"},)"
        R"({"Ip":"[fd00::1234:5678:9abc:def0]:41234"},{"Ip":"[fe80::4c3e:91ff:fe2a:7b10]:41234"}]})");
    c.token = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    c.account = QStringLiteral("mira.dorn@example.org");
    c.relay = QStringLiteral("default");
    return e3::pairing::formatCode(c);
}

// The QR code as the pairing dialog draws it (image://qr/, 260 px).
QImage drawn(const QString &text, int side = 260)
{
    QrImageProvider provider;
    QSize size;
    return provider.requestImage(QString::fromLatin1(QUrl::toPercentEncoding(text)), &size, QSize(side, side));
}

// Roughly what a phone sees of a screen: the code turned, smaller, on a
// grey background, softened, dimmer, and noisy.
QImage photographed(const QImage &code, qreal angle)
{
    QImage frame(960, 720, QImage::Format_RGB32);
    frame.fill(QColor(90, 90, 96));
    QPainter p(&frame);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(frame.width() / 2.0, frame.height() / 2.0);
    p.rotate(angle);
    const int side = 520;
    p.drawImage(QRect(-side / 2, -side / 2, side, side), code);
    p.end();
    // Soften: down and up again.
    frame = frame.scaled(frame.width() / 2, frame.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .scaled(frame.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QRandomGenerator rng(7);
    for (int y = 0; y < frame.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(frame.scanLine(y));
        for (int x = 0; x < frame.width(); ++x) {
            const int noise = int(rng.bounded(41)) - 20;
            const int v = qBound(0, qGray(line[x]) * 7 / 10 + 40 + noise, 255);
            line[x] = qRgb(v, v, v);
        }
    }
    return frame;
}

} // namespace

class TestQrScan : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // What the pairing dialog shows is read back exactly.
    void drawnCodeReadsBack()
    {
        const QString code = longCode();
        QVERIFY(e3::pairing::parseCode(code).has_value());
        const QImage img = drawn(code);
        QVERIFY(!img.isNull());
        QCOMPARE(qrdecode::pairingCode(img), code);
        QCOMPARE(e3::pairing::parseCode(qrdecode::pairingCode(img))->account, QStringLiteral("mira.dorn@example.org"));
    }

    void photographedCodeReadsBack_data()
    {
        QTest::addColumn<qreal>("angle");
        QTest::newRow("straight") << 0.0;
        QTest::newRow("15 degrees") << 15.0;
        QTest::newRow("quarter turn") << 90.0;
        QTest::newRow("upside down") << 180.0;
    }
    void photographedCodeReadsBack()
    {
        QFETCH(qreal, angle);
        const QString code = longCode();
        QCOMPARE(qrdecode::pairingCode(photographed(drawn(code, 1040), angle)), code);
    }

    // A dark theme screen, or a phone showing it inverted.
    void invertedCodeReadsBack()
    {
        const QString code = longCode();
        QImage img = drawn(code);
        img.invertPixels();
        QCOMPARE(qrdecode::pairingCode(img), code);
    }

    // Other QR codes are read, but are not pairing codes.
    void otherCodesAreIgnored()
    {
        const QImage img = drawn(QStringLiteral("https://example.org/"));
        QCOMPARE(qrdecode::decode(img), QStringLiteral("https://example.org/"));
        QVERIFY(qrdecode::pairingCode(img).isEmpty());
    }

    // A SecureJoin code (ADR 0021), photographed at an angle, is read by a
    // scanner that asks for one, and is not a pairing code.
    void contactCodeReadsBack()
    {
        const e3::mail::SecureJoin::Invite inv{QStringLiteral("0123456789ABCDEF0123456789ABCDEF01234567"),
                                               QStringLiteral("mira.dorn@example.org"), QStringLiteral("Mira Dorn"),
                                               QStringLiteral("AbCdEfGhIjKlMnOpQrStUvWx"),
                                               QStringLiteral("zYxWvUtSrQpOnMlKjIhGfEdC")};
        const QString code = e3::mail::SecureJoin::format(inv);
        const QImage img = photographed(drawn(code, 1040), 15.0);
        const QStringList prefixes{QStringLiteral("OPENPGP4FPR:"), QStringLiteral("https://i.delta.chat")};
        QCOMPARE(qrdecode::codeWithPrefix(img, prefixes), code);
        QCOMPARE(e3::mail::SecureJoin::parse(qrdecode::codeWithPrefix(img, prefixes))->name, QStringLiteral("Mira Dorn"));
        QVERIFY(qrdecode::pairingCode(img).isEmpty());
        QVERIFY(qrdecode::codeWithPrefix(drawn(longCode()), prefixes).isEmpty());
    }

    void nothingToRead()
    {
        QVERIFY(qrdecode::pairingCode(QImage()).isEmpty());
        QImage blank(640, 480, QImage::Format_RGB32);
        blank.fill(Qt::gray);
        QVERIFY(qrdecode::pairingCode(blank).isEmpty());
    }
};

QTEST_MAIN(TestQrScan)
#include "tst_qrscan.moc"
