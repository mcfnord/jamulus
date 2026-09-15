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
    Q_INVOKABLE void checkAndLookup(const QHostAddress& addr);
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

    // FU210. Both log sites below are reached once per PACKET -- socket.cpp:567 calls
    // shouldAllow() for every datagram and a client sends ~187/s -- and the cache stays missing
    // until the async lookup returns. Measured 2026-09-15 on 130.61.155.141: 109,770 lines from
    // 528 real lookups (208x), 58.5% of that host's journal, with 50.116.25.151 down to under a
    // day of retention. Suppressed events are COUNTED and reported, never silently dropped: a
    // real lookup storm must stay visible.
    QHash<QString, int> m_coalescedCount;        // guarded by m_pendingMutex
    QHash<QString, QDateTime> m_missLogged;      // guarded by m_blockedCacheMutex
    int m_missLogIntervalSeconds = 60;

    QHash<QString, QDateTime> m_blockedCache;
    QHash<QString, QDateTime> m_allowedCache;
    QMutex m_blockedCacheMutex;
    int m_blockedCacheTtlSeconds = 300;
    int m_allowedCacheTtlSeconds = 300; // match blocked TTL; caps a stale admit at 5 min
    int m_allowedCacheTtlJitterSeconds = 30;
};
