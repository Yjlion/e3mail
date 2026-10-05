// SPDX-License-Identifier: MPL-2.0
#include "NetworkBarrier.h"

#include <QNetworkReply>
#include <QTimer>

#include <atomic>

namespace {

std::atomic<int> s_blocked{0};

class RefusedReply : public QNetworkReply
{
public:
    RefusedReply(const QNetworkRequest &req, QNetworkAccessManager::Operation op, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(req);
        setUrl(req.url());
        setOperation(op);
        open(QIODevice::ReadOnly);
        setError(QNetworkReply::ContentAccessDenied, QStringLiteral("e3mail does not load remote content"));
        QTimer::singleShot(0, this, [this] {
            Q_EMIT errorOccurred(QNetworkReply::ContentAccessDenied);
            setFinished(true);
            Q_EMIT finished();
        });
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return 0; }

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

} // namespace

int BlockingNetworkAccessManager::blockedCount()
{
    return s_blocked;
}

QNetworkReply *BlockingNetworkAccessManager::createRequest(Operation op, const QNetworkRequest &request,
                                                           QIODevice *outgoingData)
{
    const QString scheme = request.url().scheme();
    if (scheme == QLatin1String("qrc") || scheme == QLatin1String("file") || scheme.isEmpty())
        return QNetworkAccessManager::createRequest(op, request, outgoingData);
    ++s_blocked;
    qWarning("blocked a remote request from the UI: %s", qPrintable(request.url().host()));
    return new RefusedReply(request, op, this);
}
