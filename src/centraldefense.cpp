#include "centraldefense.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDebug>
#include <QLoggingCategory>
#include <QFile>
#include <QTextStream>
#include <QEventLoop>

Q_LOGGING_CATEGORY(lcCentralDefense, "jamulus.centraldefense")

CentralDefense::CentralDefense(const QUrl& lookupUrl, QObject* parent)
    : QObject(parent),
      m_lookupUrl(lookupUrl)
{
    m_nam = new QNetworkAccessManager(this);
}

CentralDefense::~CentralDefense()
{
    stop();
    if (m_inflightReply) { m_inflightReply->abort(); m_inflightReply->deleteLater(); m_inflightReply = nullptr; }
    if (m_inflightTimeoutTimer) { m_inflightTimeoutTimer->stop(); m_inflightTimeoutTimer->deleteLater(); m_inflightTimeoutTimer = nullptr; }
}

void CentralDefense::loadAllowlist(const QString& path)
{
    m_allowlist.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCInfo(lcCentralDefense) << "allowlist: no file at" << path;
        return;
    }
    QTextStream in(&file);
    int count = 0;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('!')) line = line.mid(1).trimmed();
        if (line.contains('/')) {
            QStringList parts = line.split('/');
            QHostAddress addr(parts[0]);
            bool ok;
            int prefix = parts[1].toInt(&ok);
            if (!addr.isNull() && ok && prefix >= 0 && prefix <= 32) {
                m_allowlist.append({addr, prefix});
                ++count;
            } else {
                qCWarning(lcCentralDefense) << "allowlist: invalid CIDR" << line;
            }
        } else {
            QHostAddress addr(line);
            if (!addr.isNull()) {
                m_allowlist.append({addr, 32});
                ++count;
            } else {
                qCWarning(lcCentralDefense) << "allowlist: invalid address" << line;
            }
        }
    }
    qCInfo(lcCentralDefense) << "allowlist: loaded" << count << "entries from" << path;
}

bool CentralDefense::isAllowlisted(const QHostAddress& addr) const
{
    for (const auto& entry : m_allowlist) {
        if (addr.isInSubnet(entry.first, entry.second)) return true;
    }
    return false;
}

void CentralDefense::start()
{
    qCInfo(lcCentralDefense) << "starting: lookup_url" << m_lookupUrl.toString();
}

void CentralDefense::stop()
{
    qCInfo(lcCentralDefense) << "stopping";
}

void CentralDefense::checkAndLookup(const QHostAddress& addr)
{
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
        emit addressChecked(addr, false, QStringLiteral("ipv6-unsupported"));
        return;
    }

    if (isAllowlisted(addr)) {
        qCInfo(lcCentralDefense) << "lookup: local-allowlist" << addr.toString();
        emit addressChecked(addr, false, QStringLiteral("local-allowlist"));
        return;
    }

    QString ipStr = addr.toString();

    {
        QMutexLocker l(&m_blockedCacheMutex);
        auto it = m_blockedCache.find(ipStr);
        if (it != m_blockedCache.end()) {
            if (it.value().secsTo(QDateTime::currentDateTimeUtc()) < m_blockedCacheTtlSeconds) {
                qCInfo(lcCentralDefense) << "lookup: cached-block" << ipStr;
                emit addressBlocked(addr, QStringLiteral("ip-lookup-cached"));
                emit addressChecked(addr, true, QStringLiteral("ip-lookup-cached"));
                return;
            }
            m_blockedCache.erase(it);
        }
        auto ait = m_allowedCache.find(ipStr);
        if (ait != m_allowedCache.end()) {
            if (ait.value().secsTo(QDateTime::currentDateTimeUtc()) < m_allowedCacheTtlSeconds) {
                qCInfo(lcCentralDefense) << "lookup: cached-allow" << ipStr;
                emit addressChecked(addr, false, QStringLiteral("ip-lookup-cached-ok"));
                return;
            }
            m_allowedCache.erase(ait);
        }
    }

    {
        QMutexLocker l(&m_pendingMutex);

        if (m_inflightIp == ipStr) {
            qCInfo(lcCentralDefense) << "lookup: coalesced (inflight)" << ipStr;
            return;
        }
        if (m_pendingSet.contains(ipStr)) {
            qCInfo(lcCentralDefense) << "lookup: coalesced (queued)" << ipStr;
            return;
        }

        if (m_pendingQueue.size() >= m_maxPending) {
            qCWarning(lcCentralDefense) << "lookup: dropped (queue full)" << ipStr;
            emit addressChecked(addr, false, QStringLiteral("queue-full"));
            return;
        }

        m_pendingQueue.enqueue(ipStr);
        m_pendingSet.insert(ipStr);
        qCInfo(lcCentralDefense) << "lookup: enqueue" << ipStr << "queue_len=" << m_pendingQueue.size();
    }

    if (!m_inflightReply) startNextLookup();
}

void CentralDefense::startNextLookup()
{
    if (m_inflightReply) return;

    QString nextIp;
    {
        QMutexLocker l(&m_pendingMutex);
        if (m_pendingQueue.isEmpty()) return;
        nextIp = m_pendingQueue.dequeue();
        m_pendingSet.remove(nextIp);
    }

    if (nextIp.isEmpty()) return;

    QUrl url = m_lookupUrl;
    QString path = url.path();
    if (!path.endsWith('/')) path += '/';
    path += nextIp;
    url.setPath(path);

    qCInfo(lcCentralDefense) << "lookup: start" << nextIp << "url=" << url.toString();
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-CentralDefense/1.0"));

    QNetworkReply* reply = m_nam->get(req);
    m_inflightReply = reply;
    m_inflightIp = nextIp;

    connect(reply, &QNetworkReply::finished, this, &CentralDefense::onLookupFinished);

    if (m_inflightTimeoutTimer) { m_inflightTimeoutTimer->stop(); m_inflightTimeoutTimer->deleteLater(); m_inflightTimeoutTimer = nullptr; }
    m_inflightTimeoutTimer = new QTimer(this);
    m_inflightTimeoutTimer->setSingleShot(true);
    connect(m_inflightTimeoutTimer, &QTimer::timeout, this, [reply]() {
        if (reply && reply->isRunning()) reply->abort();
    });
    m_inflightTimeoutTimer->start(m_lookupTimeoutSeconds * 1000);
}

void CentralDefense::onLookupFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    if (m_inflightTimeoutTimer) {
        m_inflightTimeoutTimer->stop();
        m_inflightTimeoutTimer->deleteLater();
        m_inflightTimeoutTimer = nullptr;
    }

    QString ip = m_inflightIp;
    m_inflightReply = nullptr;
    m_inflightIp.clear();

    QHostAddress addr(ip);

    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(lcCentralDefense) << "lookup: error" << ip << reply->errorString();
        emit addressChecked(addr, false, QStringLiteral("lookup-failed"));
        reply->deleteLater();
        QTimer::singleShot(m_lookupStartSpacingMs, this, &CentralDefense::startNextLookup);
        return;
    }

    QString body = QString::fromUtf8(reply->readAll()).trimmed();
    reply->deleteLater();

    bool blocked = (body == QStringLiteral("false"));
    qCInfo(lcCentralDefense) << "lookup: finish" << ip << "blocked=" << blocked;

    if (blocked) {
        {
            QMutexLocker l(&m_blockedCacheMutex);
            m_blockedCache.insert(ip, QDateTime::currentDateTimeUtc());
        }
        emit addressBlocked(addr, QStringLiteral("ip-lookup"));
        emit addressChecked(addr, true, QStringLiteral("ip-lookup"));
    } else {
        {
            QMutexLocker l(&m_blockedCacheMutex);
            m_allowedCache.insert(ip, QDateTime::currentDateTimeUtc());
        }
        emit addressChecked(addr, false, QStringLiteral("ip-lookup-ok"));
    }

    QTimer::singleShot(m_lookupStartSpacingMs, this, &CentralDefense::startNextLookup);
}

bool CentralDefense::shouldAllow(const QHostAddress& addr)
{
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) return true;
    if (isAllowlisted(addr)) return true;

    QString ipStr = addr.toString();

    {
        QMutexLocker l(&m_blockedCacheMutex);
        auto it = m_blockedCache.find(ipStr);
        if (it != m_blockedCache.end()) {
            if (it.value().secsTo(QDateTime::currentDateTimeUtc()) < m_blockedCacheTtlSeconds)
                return false;
            m_blockedCache.erase(it);
        }
        auto ait = m_allowedCache.find(ipStr);
        if (ait != m_allowedCache.end()) {
            if (ait.value().secsTo(QDateTime::currentDateTimeUtc()) < m_allowedCacheTtlSeconds)
                return true;
            m_allowedCache.erase(ait);
        }
    }

    QUrl url = m_lookupUrl;
    QString path = url.path();
    if (!path.endsWith('/')) path += '/';
    path += ipStr;
    url.setPath(path);

    qCInfo(lcCentralDefense) << "precheck: lookup" << ipStr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-CentralDefense/1.0"));

    QNetworkReply* reply = m_nam->get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(m_lookupTimeoutSeconds * 1000);
    loop.exec();

    bool allowed = true;
    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(lcCentralDefense) << "precheck: error (fail-open)" << ipStr << reply->errorString();
        QMutexLocker l(&m_blockedCacheMutex);
        m_allowedCache.insert(ipStr, QDateTime::currentDateTimeUtc());
    } else {
        QString body = QString::fromUtf8(reply->readAll()).trimmed();
        bool blocked = (body == QStringLiteral("false"));
        allowed = !blocked;
        qCInfo(lcCentralDefense) << "precheck: finish" << ipStr << "blocked=" << blocked;
        QMutexLocker l(&m_blockedCacheMutex);
        if (blocked)
            m_blockedCache.insert(ipStr, QDateTime::currentDateTimeUtc());
        else
            m_allowedCache.insert(ipStr, QDateTime::currentDateTimeUtc());
    }
    reply->deleteLater();
    return allowed;
}
