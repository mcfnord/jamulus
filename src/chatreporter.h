#pragma once

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMutex>
#include <QList>
#include <QRegularExpression>
#include <functional>

class QWebSocket;

class ChatReporter : public QObject
{
    Q_OBJECT

public:
    explicit ChatReporter(const QUrl& patternUrl, const QUrl& reportUrl, quint16 port, QObject* parent = nullptr);

    void start();
    void reportIfMatch(const QString& text);
    // For chat a CLIENT received. The server stamps every message a channel sent with
    // "<font color=...>(time) <b>name</b></font> "; anything without that stamp was
    // injected by the server itself -- the welcome message (prefixed on stock servers,
    // raw HTML when JamFan22 pushes one over the RPC channel), or an [Ear] announcement.
    // Only stamped messages are chat sent during this session, so only they are reported.
    void reportIfMatchFromChat(const QString& formattedText);
    void reportSongIfMatch(const QString& rawText);
    void checkCommand(const QString& text, int port, const QHostAddress& clientAddr = QHostAddress());
    void reportClientInfo(const QHostAddress& addr, const QString& name, int countryId, int instrument, int channelId);
    void setWelcomeCallback(std::function<void(int, const QString&)> cb) { m_welcomeCallback = std::move(cb); }
    void setRpcDispatch(std::function<QString(const QJsonObject&)> cb) { m_rpcDispatch = std::move(cb); }
    void setServerAddr(const QString& addr) { m_serverAddr = addr; }

    // Client builds report a chat URL only when the user leaves this on. Client patterns are
    // compiled in and there is no remote fetch, so a disabled reporter makes no network calls
    // at all -- the flag is checked at the single entry point, reportIfMatch().
    void setEnabled(bool bEna) { m_enabled = bEna; }
    bool isEnabled() const { return m_enabled; }

signals:
    void commandResponse(const QString& text);

private slots:
    void onPatternsFetched();
    void refreshPatterns();
    void onFleetMessage(const QString& text);
    void onFleetDisconnected();
    void onFleetPong(quint64 elapsedTime, const QByteArray& payload);
    void onFleetPongTimeout();

private:
    void fetchPatterns();
    void postUrl(const QString& url);
    void postSong(const QString& title);
    void connectFleetSocket();
    // Heartbeat over the fleet RPC channel. Without it the ONLY thing that triggers a
    // reconnect is QWebSocket::disconnected, and a path that dies without a FIN or RST
    // never emits it -- both ends sit ESTABLISHED and the room is silently unreachable
    // (measured 2026-09-12: Spectre Rising dropped every welcome for 24.4 h this way).
    void startFleetHeartbeat();
    void stopFleetHeartbeat();
    void scheduleFleetReconnect();

    QUrl m_patternUrl;
    QUrl m_reportUrl;
    QUrl m_commandUrl;
    quint16 m_port    = 0;
    QString m_serverAddr;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_refreshTimer = nullptr;
    bool m_enabled = true;
    QWebSocket* m_fleetSocket = nullptr;
    int m_fleetReconnectMs = 5000;
    QTimer* m_fleetPingTimer = nullptr;
    QTimer* m_fleetPongTimer = nullptr;

    std::function<void(int, const QString&)> m_welcomeCallback;
    std::function<QString(const QJsonObject&)> m_rpcDispatch;

    QMutex m_patternMutex;
    QList<QRegularExpression> m_patterns;

    static constexpr int FETCH_TIMEOUT_MS = 5000;
    static constexpr int FLEET_RECONNECT_MAX_MS = 60000;
    // 30 s does double duty: it detects a dead channel in ~45 s instead of never, AND keeps
    // the NAT mapping alive, so it should PREVENT the idle-timeout drop rather than only
    // catching it. Slower than a few seconds on purpose -- the failure it replaces took
    // 24.4 h to notice, and this multiplies by every room on every host.
    static constexpr int FLEET_PING_INTERVAL_MS = 30000;
    static constexpr int FLEET_PONG_TIMEOUT_MS = 15000;
};
