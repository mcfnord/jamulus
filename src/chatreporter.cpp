#include "chatreporter.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcChatReporter, "jamulus.chatreporter")

static constexpr int PATTERN_REFRESH_MS = 60 * 60 * 1000; // 1 hour

ChatReporter::ChatReporter(const QUrl& patternUrl, const QUrl& reportUrl, QObject* parent)
    : QObject(parent),
      m_patternUrl(patternUrl),
      m_reportUrl(reportUrl)
{
    m_nam = new QNetworkAccessManager(this);
}

void ChatReporter::start()
{
    qCInfo(lcChatReporter) << "starting: pattern_url=" << m_patternUrl.toString()
                           << "report_url=" << m_reportUrl.toString();
    fetchPatterns();

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(PATTERN_REFRESH_MS);
    connect(m_refreshTimer, &QTimer::timeout, this, &ChatReporter::refreshPatterns);
    m_refreshTimer->start();
}

void ChatReporter::fetchPatterns()
{
    QNetworkRequest req(m_patternUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(FETCH_TIMEOUT_MS);
#endif
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, &ChatReporter::onPatternsFetched);
}

void ChatReporter::refreshPatterns()
{
    qCInfo(lcChatReporter) << "refreshing patterns";
    fetchPatterns();
}

void ChatReporter::onPatternsFetched()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qCWarning(lcChatReporter) << "failed to fetch patterns:" << reply->errorString();
        return;
    }

    QList<QRegularExpression> newPatterns;
    const QString body = QString::fromUtf8(reply->readAll());
    for (const QString& rawLine : body.split('\n')) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        QRegularExpression re(line);
        if (!re.isValid()) {
            qCWarning(lcChatReporter) << "invalid pattern:" << line << re.errorString();
            continue;
        }
        newPatterns.append(re);
    }

    {
        QMutexLocker l(&m_patternMutex);
        m_patterns = newPatterns;
    }
    qCInfo(lcChatReporter) << "loaded" << newPatterns.size() << "patterns";
}

void ChatReporter::reportIfMatch(const QString& text)
{
    QList<QRegularExpression> patterns;
    {
        QMutexLocker l(&m_patternMutex);
        patterns = m_patterns;
    }

    QSet<QString> reported;
    for (const QRegularExpression& re : patterns) {
        QRegularExpressionMatchIterator it = re.globalMatch(text);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            // Use first capture group if present, otherwise the full match
            QString url = m.lastCapturedIndex() >= 1 ? m.captured(1) : m.captured(0);
            if (!url.isEmpty() && !reported.contains(url)) {
                reported.insert(url);
                postUrl(url);
            }
        }
    }
}

void ChatReporter::postUrl(const QString& url)
{
    qCInfo(lcChatReporter) << "reporting url:" << url;

    QNetworkRequest req(m_reportUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QJsonObject body;
    body["url"] = url;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_nam->post(req, payload);
    // Fire and forget — clean up on finish, ignore errors
    connect(reply, &QNetworkReply::finished, reply, [reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            // intentionally silent
        }
        reply->deleteLater();
    });
}
