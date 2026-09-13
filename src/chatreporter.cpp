#include "chatreporter.h"
#include "jamuluslookups.h"
#include "global.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QLoggingCategory>
#include <QCryptographicHash>
#include <QWebSocket>

Q_LOGGING_CATEGORY(lcChatReporter, "jamulus.chatreporter")

static constexpr int PATTERN_REFRESH_MS = 60 * 60 * 1000; // 1 hour

ChatReporter::ChatReporter(const QUrl& patternUrl, const QUrl& reportUrl, quint16 port, QObject* parent)
    : QObject(parent),
      m_patternUrl(patternUrl),
      m_reportUrl(reportUrl),
      m_commandUrl(QStringLiteral("https://jamulus.live/chat-command-server")),
      m_port(port)
{
    m_nam = new QNetworkAccessManager(this);
}

void ChatReporter::start()
{
    qCInfo(lcChatReporter) << "starting: report_url=" << m_reportUrl.toString();

#ifdef SERVER_BUNDLE
    qCInfo(lcChatReporter) << "pattern_url=" << m_patternUrl.toString();
    fetchPatterns();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(PATTERN_REFRESH_MS);
    connect(m_refreshTimer, &QTimer::timeout, this, &ChatReporter::refreshPatterns);
    m_refreshTimer->start();
#else
    // Client builds: patterns are compiled in. No remote fetch, no remote override.
    // Changing what URLs are reported requires building and distributing a new binary.
    static const char* const kPatterns[] = {
        R"(https://vdo\.ninja/[^\s]*)",
        R"(https://meet\.google\.com/[^\s]*)",
        R"(https://[\w\-]+\.zoom\.us/[^\s]*)",
        R"(https://meet\.jit\.si/[^\s]*)",
        R"(https://busk\.town/[^\s]*)",
        R"(https://chordtabs\.in\.th/[^\s]*)",
        R"(https://designbetrieb\.de/[^\s]*)",
        R"(https://www\.follner-music\.de/jamu/[^\s]*)",
        R"(https://(?:[\w-]+\.)?ultimate-guitar\.com/[^\s]*)",
        R"(https://chords69cl\.vercel\.app/[^\s]*)",
        R"(https://www\.guitarthai\.com/[^\s]*)",
        R"(https://www\.dochord\.com/[^\s]*)",
        R"(https://www\.virtualsheetmusic\.com/[^\s]*)",
        R"(https://(?:[\w-]+\.)?guitarians\.com/[^\s]*)",
        R"(https://vocal-voyage\.de/[^\s]*)",
        nullptr
    };
    {
        QMutexLocker l(&m_patternMutex);
        for (int i = 0; kPatterns[i]; ++i) {
            QRegularExpression re(QString::fromLatin1(kPatterns[i]));
            if (re.isValid())
                m_patterns.append(re);
        }
    }
    qCInfo(lcChatReporter) << "loaded" << m_patterns.size() << "compiled-in patterns";
#endif

    connectFleetSocket();
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

void ChatReporter::reportIfMatchFromChat(const QString& formattedText)
{
    // Measured 2026-09-11 against an upstream server and the live lounge:
    //   chat    "<font color=\"mediumblue\">(02:37:01 PM) <b>fmt probe</b></font> check this https://..."
    //   welcome "<b>Server Welcome Message:</b> Our video room: https://..."   (stock server)
    //   welcome "<br><big>You've joined Hot Texas!</big><br><a href=...>"      (pushed by JamFan22)
    // Only the first is a message somebody sent during this session.
    static const QRegularExpression chatStampRe(
        QStringLiteral(R"(^<font color="[^"]*">\([^<]*\) <b>.*</b></font> )"));

    if (!chatStampRe.match(formattedText).hasMatch())
        return;

    reportIfMatch(formattedText);
}

void ChatReporter::reportIfMatch(const QString& text)
{
    if (!m_enabled)
        return;

    // The server sends its welcome message as a chat text message to every client the
    // moment it connects, so a URL sitting in that message would be reported on every
    // single join and show up as if someone had just posted it. Only chat sent during
    // the session counts. This is the same test clientdlg.cpp uses to spot the welcome.
    if (text.startsWith(QLatin1String(WELCOME_MESSAGE_PREFIX)))
        return;

    static const QRegularExpression urlRe(QStringLiteral(R"(https?://[^\s<>"']+)"),
                                          QRegularExpression::CaseInsensitiveOption);

#ifndef SERVER_BUNDLE
    QList<QRegularExpression> patterns;
    {
        QMutexLocker l(&m_patternMutex);
        patterns = m_patterns;
    }
    if (patterns.isEmpty())
        return;
#endif

    QSet<QString> reported;
    QRegularExpressionMatchIterator it = urlRe.globalMatch(text);
    while (it.hasNext()) {
        const QString url = it.next().captured(0);
        if (url.isEmpty() || reported.contains(url))
            continue;
#ifndef SERVER_BUNDLE
        bool matched = false;
        for (const QRegularExpression& re : patterns)
            if (re.match(url).hasMatch()) { matched = true; break; }
        if (!matched)
            continue;
#endif
        reported.insert(url);
        postUrl(url);
    }
}

void ChatReporter::checkCommand(const QString& text, int port, const QHostAddress& clientAddr)
{
    if (!text.trimmed().startsWith(QStringLiteral("/stream")))
        return;

    QNetworkRequest req(m_commandUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QJsonObject body;
    body[QStringLiteral("command")] = QStringLiteral("stream");
    body[QStringLiteral("port")] = port;
    body[QStringLiteral("weekly")] = false;
    if (!clientAddr.isNull())
        body[QStringLiteral("clientIp")] = clientAddr.toString();
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_nam->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            const QString response = QString::fromUtf8(reply->readAll()).trimmed();
            if (!response.isEmpty())
                emit commandResponse(response);
        }
        reply->deleteLater();
    });
}

void ChatReporter::reportClientInfo(const QHostAddress& addr, const QString& name, int countryId, int instrument, int channelId)
{
    if (addr.isNull() || addr == QHostAddress(static_cast<quint32>(0)))
        return;

    QByteArray input = (name + phpCountryName(countryId) + phpInstrumentName(instrument)).toUtf8();
    QString guid = QCryptographicHash::hash(input, QCryptographicHash::Md5).toHex();

    qCInfo(lcChatReporter) << "GUID_IP guid=" << guid << "ip=" << addr.toString() << "channel=" << channelId << "name=" << name;

    QUrl url(QStringLiteral("https://jamulus.live/player-identified/") + addr.toString());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("serverport"), QString::number(m_port));
    query.addQueryItem(QStringLiteral("guid"), guid);
    query.addQueryItem(QStringLiteral("channelId"), QString::number(channelId));
    QString nation = QLocale(QLocale::AnyLanguage, QLocale::AnyScript, static_cast<QLocale::Country>(countryId)).name().split('_').last();
    if (!nation.isEmpty() && nation != QLatin1String("C"))
        query.addQueryItem(QStringLiteral("nation"), nation);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void ChatReporter::reportSongIfMatch(const QString& rawText)
{
    // Matches "<title> – Dm" or "<title> — F#m" (en/em dash only; hyphen-minus excluded to avoid false positives)
    static const QRegularExpression songRe(
        QStringLiteral(R"(^(.*\S)\s*[–—]\s*[A-G][#b]?m?\s*$)"));

    QRegularExpressionMatch m = songRe.match(rawText.trimmed());
    if (!m.hasMatch())
        return;

    const QString title = m.captured(1).trimmed();
    if (title.isEmpty())
        return;

    postSong(title);
}

void ChatReporter::postSong(const QString& title)
{
    qCInfo(lcChatReporter) << "reporting song:" << title;

    static const QUrl songUrl(QStringLiteral("https://jamulus.live/chat-song-server"));
    QNetworkRequest req(songUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QJsonObject body;
    body[QStringLiteral("title")] = title;
    body[QStringLiteral("port")]  = m_port;
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_nam->post(req, payload);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void ChatReporter::postUrl(const QString& url)
{
    qCInfo(lcChatReporter) << "reporting url:" << url;

    QNetworkRequest req(m_reportUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QJsonObject body;
    body["url"] = url;
    body["port"] = m_port;
    if (!m_serverAddr.isEmpty())
        body["serverAddr"] = m_serverAddr;
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

void ChatReporter::connectFleetSocket()
{
    stopFleetHeartbeat();

    if (m_fleetSocket) {
        m_fleetSocket->disconnect(this);
        m_fleetSocket->abort();
        m_fleetSocket->deleteLater();
        m_fleetSocket = nullptr;
    }

    m_fleetSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_fleetSocket, &QWebSocket::textMessageReceived, this, &ChatReporter::onFleetMessage);
    connect(m_fleetSocket, &QWebSocket::disconnected, this, &ChatReporter::onFleetDisconnected);
    connect(m_fleetSocket, &QWebSocket::pong, this, &ChatReporter::onFleetPong);
    connect(m_fleetSocket, &QWebSocket::connected, this, [this]() {
        qCInfo(lcChatReporter) << "[fleet-rpc-channel] connected port=" << m_port;
        m_fleetReconnectMs = 5000;
        startFleetHeartbeat();
    });

    QUrl url(QStringLiteral("wss://jamulus.live/fleet-rpc-channel"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("port"), QString::number(m_port));
#ifndef SERVER_BUNDLE
    // Client builds register the build string so a field trial can see which
    // testers are live and on which binary. Server builds are identified by
    // their real port and add nothing.
    q.addQueryItem(QStringLiteral("build"), QStringLiteral(APP_VERSION));
#endif
    url.setQuery(q);

    m_fleetSocket->open(url);
}

void ChatReporter::onFleetMessage(const QString& text)
{
    if (!m_rpcDispatch || !m_fleetSocket)
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isObject())
        m_fleetSocket->sendTextMessage(m_rpcDispatch(doc.object()));
}

void ChatReporter::onFleetDisconnected()
{
    // Log text is byte-identical to what it has always been: anything grepping this
    // marker in the journal keeps working, and 'disconnected' contains 'connected' as a
    // substring (mistake 40), so paired-marker parsers must not see a new spelling.
    qCInfo(lcChatReporter) << "[fleet-rpc-channel] disconnected — reconnecting in" << m_fleetReconnectMs << "ms";
    scheduleFleetReconnect();
}

void ChatReporter::startFleetHeartbeat()
{
    if (!m_fleetPingTimer) {
        m_fleetPingTimer = new QTimer(this);
        m_fleetPingTimer->setInterval(FLEET_PING_INTERVAL_MS);
        connect(m_fleetPingTimer, &QTimer::timeout, this, [this]() {
            if (!m_fleetSocket)
                return;
            // One ping outstanding at a time: the deadline timer is armed here and
            // disarmed by the pong, so a second ping never hides a missing first one.
            if (m_fleetPongTimer->isActive())
                return;
            m_fleetSocket->ping();
            m_fleetPongTimer->start(FLEET_PONG_TIMEOUT_MS);
        });
    }
    if (!m_fleetPongTimer) {
        m_fleetPongTimer = new QTimer(this);
        m_fleetPongTimer->setSingleShot(true);
        connect(m_fleetPongTimer, &QTimer::timeout, this, &ChatReporter::onFleetPongTimeout);
    }
    m_fleetPongTimer->stop();
    m_fleetPingTimer->start();
}

void ChatReporter::stopFleetHeartbeat()
{
    if (m_fleetPingTimer)
        m_fleetPingTimer->stop();
    if (m_fleetPongTimer)
        m_fleetPongTimer->stop();
}

void ChatReporter::onFleetPong(quint64 elapsedTime, const QByteArray&)
{
    Q_UNUSED(elapsedTime)
    if (m_fleetPongTimer)
        m_fleetPongTimer->stop();
}

void ChatReporter::onFleetPongTimeout()
{
    qCWarning(lcChatReporter) << "[fleet-rpc-channel] no pong within" << FLEET_PONG_TIMEOUT_MS
                              << "ms — channel dead, rebuilding socket";

    // Rebuild the SOCKET, never restart the process: a control-channel fault must not
    // kick players off the audio path (convention 8). Detach our handlers first so
    // abort() cannot re-enter onFleetDisconnected() and double-schedule a reconnect.
    if (m_fleetSocket) {
        m_fleetSocket->disconnect(this);
        m_fleetSocket->abort();
    }
    scheduleFleetReconnect();
}

void ChatReporter::scheduleFleetReconnect()
{
    // Always route through the existing backoff, never straight to connectFleetSocket():
    // a jamulus.live restart times out every room on every host at the same moment, and
    // this backoff is the only thing between that and a fleet-wide redial storm.
    stopFleetHeartbeat();
    QTimer::singleShot(m_fleetReconnectMs, this, &ChatReporter::connectFleetSocket);
    m_fleetReconnectMs = qMin(m_fleetReconnectMs * 2, FLEET_RECONNECT_MAX_MS);
}

