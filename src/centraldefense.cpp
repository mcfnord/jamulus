#include "centraldefense.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDebug>
#include <QLoggingCategory>

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

    QString ipStr = addr.toString();

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

    bool blocked = (body == QStringLiteral("true"));
    qCInfo(lcCentralDefense) << "lookup: finish" << ip << "blocked=" << blocked;

    if (blocked) {
        emit addressBlocked(addr, QStringLiteral("ip-lookup"));
        emit addressChecked(addr, true, QStringLiteral("ip-lookup"));
    } else {
        emit addressChecked(addr, false, QStringLiteral("ip-lookup-ok"));
    }

    QTimer::singleShot(m_lookupStartSpacingMs, this, &CentralDefense::startNextLookup);
}
