#pragma once

#include <QObject>
#include <QUrl>
#include <QHostAddress>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QSet>
#include <QMutex>
#include <QHash>
#include <QDateTime>
#include <QList>
#include <QPair>

class CentralDefense : public QObject
{
    Q_OBJECT

public:
    explicit CentralDefense(const QUrl& lookupUrl, QObject* parent = nullptr);
    ~CentralDefense() override;

    void start();
    void stop();
    void checkAndLookup(const QHostAddress& addr);
    void loadAllowlist(const QString& path);
    bool shouldAllow(const QHostAddress& addr);

signals:
    void addressChecked(const QHostAddress& addr, bool isBlocked, const QString& reason);
    void addressBlocked(const QHostAddress& addr, const QString& reason);
    void updated(int numAsns, int numCidrs);

private slots:
    void onLookupFinished();

private:
    void startNextLookup();
    bool isAllowlisted(const QHostAddress& addr) const;

    QList<QPair<QHostAddress, int>> m_allowlist; // address + prefix length

    QUrl m_lookupUrl;
    QNetworkAccessManager* m_nam = nullptr;

    QQueue<QString> m_pendingQueue;
    QSet<QString> m_pendingSet;
    QMutex m_pendingMutex;
    int m_maxPending = 25;

    QNetworkReply* m_inflightReply = nullptr;
    QString m_inflightIp;
    QTimer* m_inflightTimeoutTimer = nullptr;
    int m_lookupStartSpacingMs = 1000;
    int m_lookupTimeoutSeconds = 2;

    QHash<QString, QDateTime> m_blockedCache;
    QHash<QString, QDateTime> m_allowedCache;
    QMutex m_blockedCacheMutex;
    int m_blockedCacheTtlSeconds = 300;
    int m_allowedCacheTtlSeconds = 3600;
};
