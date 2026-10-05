// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QNetworkAccessManager>
#include <QQmlNetworkAccessManagerFactory>

// The second of two barriers between message content and the network. The
// sanitizer already strips remote references; independently of it, the QML
// engine that renders mail gets a network manager that refuses every request
// that is not to the application's own resources. A mistake in either layer
// alone must not let a message phone home.
class BlockingNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT
public:
    using QNetworkAccessManager::QNetworkAccessManager;
    static int blockedCount();

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override;
};

class NetworkBarrierFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override { return new BlockingNetworkAccessManager(parent); }
};
