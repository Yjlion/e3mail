// SPDX-License-Identifier: MPL-2.0
#include "FakeOAuthServer.h"
#include "SignIn.h"
#include "net/OAuth.h"
#include "net/Sasl.h"
#include "net/SyncSocket.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest>

using namespace e3;

namespace {

// What a browser does after the person agrees: load the redirect URI with
// the code and state the provider gave. Returns the page's status.
int browserReturns(const QUrl &authorizeUrl, const QString &code, const QString &state)
{
    const QUrlQuery q(authorizeUrl);
    QUrl back(q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
    QUrlQuery r;
    r.addQueryItem(QStringLiteral("code"), code);
    r.addQueryItem(QStringLiteral("state"), state);
    back.setQuery(r);
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(back));
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(5000);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return status;
}

} // namespace

class TestOAuth : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    // The whole sign-in, as the app does it, against a fake provider.
    void signIn()
    {
        FakeOAuthServer provider;
        SignIn signIn(provider.provider(), QStringLiteral("dora@x.test"));
        QSignalSpy browser(&signIn, &SignIn::openBrowser);
        QSignalSpy finished(&signIn, &SignIn::finished);
        signIn.start();
        QTRY_COMPARE_WITH_TIMEOUT(browser.size(), 1, 5000);
        const QUrl url = browser.first().first().toUrl();
        const QUrlQuery q(url);
        QCOMPARE(url.path(), QStringLiteral("/authorize"));
        QCOMPARE(q.queryItemValue(QStringLiteral("client_id")), QStringLiteral("test-client"));
        QCOMPARE(q.queryItemValue(QStringLiteral("response_type")), QStringLiteral("code"));
        QCOMPARE(q.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QCOMPARE(q.queryItemValue(QStringLiteral("access_type")), QStringLiteral("offline"));
        QCOMPARE(q.queryItemValue(QStringLiteral("login_hint"), QUrl::FullyDecoded), QStringLiteral("dora@x.test"));
        QVERIFY(q.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded).contains(QLatin1String("https://mail.google.com/")));
        const QUrl redirect(q.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        QCOMPARE(redirect.host(), QStringLiteral("127.0.0.1"));

        // The person agrees: the code is exchanged, with the PKCE verifier.
        const QString challenge = q.queryItemValue(QStringLiteral("code_challenge"));
        QCOMPARE(browserReturns(url, provider.approve(challenge, QStringLiteral("dora@x.test")),
                                q.queryItemValue(QStringLiteral("state"))), 200);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().first().toString(), QString());
        QCOMPARE(provider.exchanges, 1);
        QVERIFY(signIn.refreshToken().startsWith(QLatin1String("refresh-")));
        QVERIFY(signIn.accessToken().startsWith(QLatin1String("access-")));
        QCOMPARE(signIn.email(), QStringLiteral("dora@x.test"));
        QVERIFY(signIn.expires() > QDateTime::currentDateTimeUtc().addSecs(3000));
    }

    // An answer with another state is not this sign-in's: it ends it, and
    // nothing is exchanged for the code.
    void forgedStateFails()
    {
        FakeOAuthServer provider;
        SignIn signIn(provider.provider(), QString());
        QSignalSpy browser(&signIn, &SignIn::openBrowser);
        QSignalSpy finished(&signIn, &SignIn::finished);
        signIn.start();
        QTRY_COMPARE_WITH_TIMEOUT(browser.size(), 1, 5000);
        const QUrl url = browser.first().first().toUrl();
        const QString challenge = QUrlQuery(url).queryItemValue(QStringLiteral("code_challenge"));
        browserReturns(url, provider.approve(challenge, QStringLiteral("dora@x.test")), QStringLiteral("forged"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(!finished.first().first().toString().isEmpty());
        QCOMPARE(provider.exchanges, 0);
        QVERIFY(signIn.refreshToken().isEmpty());
    }

    void tokenSource()
    {
        FakeOAuthServer provider;
        provider.addRefreshToken(QStringLiteral("r1"), QStringLiteral("dora@x.test"));
        net::oauth::TokenSource source(provider.provider(), QStringLiteral("r1"));
        const QString first = source.accessToken();
        QCOMPARE(provider.refreshes, 1);
        QCOMPARE(source.accessToken(), first); // cached while valid
        QCOMPARE(provider.refreshes, 1);
        QVERIFY(source.accessToken(true) != first); // renewed when asked
        QCOMPARE(provider.refreshes, 2);

        // Near expiry it renews by itself.
        provider.expiresIn = 30;
        source.accessToken(true);
        source.accessToken();
        QCOMPARE(provider.refreshes, 4);

        // Rotation is reported, and the new one used from then on.
        provider.rotate = true;
        QString stored;
        source.onRefreshTokenChanged = [&stored](const QString &t) { stored = t; };
        source.accessToken(true);
        QVERIFY(!stored.isEmpty() && stored != QLatin1String("r1"));
        QCOMPARE(source.refreshToken(), stored);
        source.accessToken(true);
        QCOMPARE(provider.refreshes, 6);

        // Revoked: an AuthError that says to sign in again.
        provider.revoke(source.refreshToken());
        QVERIFY_THROWS_EXCEPTION(net::AuthError, source.accessToken(true));
    }

    void providers()
    {
        qputenv("E3MAIL_OAUTH_GOOGLE_ID", "");
        qputenv("E3MAIL_OAUTH_MICROSOFT_ID", "");
        // Without a client ID a provider is offered nowhere (unless this
        // build was given one).
        if (QByteArray(E3MAIL_TEST_BUILT_GOOGLE_ID).isEmpty())
            QVERIFY(!net::oauth::provider(QStringLiteral("google")));
        QVERIFY(net::oauth::known(QStringLiteral("google")));
        qputenv("E3MAIL_OAUTH_GOOGLE_ID", "mine.apps.googleusercontent.com");
        qputenv("E3MAIL_OAUTH_MICROSOFT_ID", "00000000-0000-0000-0000-000000000000");
        QCOMPARE(net::oauth::forDomain(QStringLiteral("gmail.com"))->id, QStringLiteral("google"));
        QCOMPARE(net::oauth::forDomain(QStringLiteral("Outlook.com"))->id, QStringLiteral("microsoft"));
        QCOMPARE(net::oauth::forDomain(QStringLiteral("hotmail.co.uk"))->id, QStringLiteral("microsoft"));
        QVERIFY(!net::oauth::forDomain(QStringLiteral("posteo.de")));
        QCOMPARE(net::oauth::forIssuer(QStringLiteral("accounts.google.com"))->clientId,
                 QStringLiteral("mine.apps.googleusercontent.com"));
        QCOMPARE(net::oauth::forIssuer(QStringLiteral("login.microsoftonline.com"))->id, QStringLiteral("microsoft"));
        qunsetenv("E3MAIL_OAUTH_GOOGLE_ID");
        qunsetenv("E3MAIL_OAUTH_MICROSOFT_ID");
    }

    void idToken()
    {
        auto jwt = [](const QByteArray &json) {
            return QStringLiteral("e30.%1.sig").arg(QString::fromLatin1(
                json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
        };
        QCOMPARE(net::oauth::emailFromIdToken(jwt(R"({"email":"a@b.test"})")), QStringLiteral("a@b.test"));
        QCOMPARE(net::oauth::emailFromIdToken(jwt(R"({"preferred_username":"c@d.test"})")), QStringLiteral("c@d.test"));
        QCOMPARE(net::oauth::emailFromIdToken(jwt(R"({"preferred_username":"not an address"})")), QString());
        QCOMPARE(net::oauth::emailFromIdToken(QStringLiteral("garbage")), QString());
    }

    // The SASL strings, byte for byte: Google's XOAUTH2 example and RFC 7628's.
    void sasl()
    {
        QCOMPARE(QByteArray::fromBase64(net::sasl::xoauth2("someuser@example.com", "ya29.vF9dft4qmTc2Nvb3RlckBhdHRhdmlzdGEuY29tCg")),
                 QByteArray("user=someuser@example.com\x01" "auth=Bearer ya29.vF9dft4qmTc2Nvb3RlckBhdHRhdmlzdGEuY29tCg\x01\x01"));
        QCOMPARE(QByteArray::fromBase64(net::sasl::oauthBearer("user@example.com", "vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==",
                                                               "server.example.com", 143)),
                 QByteArray("n,a=user@example.com,\x01" "host=server.example.com\x01" "port=143\x01"
                            "auth=Bearer vF9dft4qmTc2Nvb3RlckBhbHRhdmlzdGEuY29tCg==\x01\x01"));
        QCOMPARE(net::sasl::challengeText("+ eyJzdGF0dXMiOiI0MDEifQ=="), QByteArray(R"({"status":"401"})"));
        QCOMPARE(net::sasl::challengeText("eyJzdGF0dXMiOiI0MDEifQ=="), QByteArray(R"({"status":"401"})"));
    }
};

QTEST_GUILESS_MAIN(TestOAuth)
#include "tst_oauth.moc"
