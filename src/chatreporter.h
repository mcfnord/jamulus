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
    void reportSongIfMatch(const QString& rawText);
    void checkCommand(const QString& text, int port, const QHostAddress& clientAddr = QHostAddress());
    void reportClientInfo(const QHostAddress& addr, const QString& name, int countryId, int instrument, int channelId);
    void setWelcomeCallback(std::function<void(int, const QString&)> cb) { m_welcomeCallback = std::move(cb); }
    void setRpcDispatch(std::function<QString(const QJsonObject&)> cb) { m_rpcDispatch = std::move(cb); }
    void setServerAddr(const QString& addr) { m_serverAddr = addr; }

signals:
    void commandResponse(const QString& text);

private slots:
    void onPatternsFetched();
    void refreshPatterns();
    void onFleetMessage(const QString& text);
    void onFleetDisconnected();

private:
    void fetchPatterns();
    void postUrl(const QString& url);
    void postSong(const QString& title);
    void connectFleetSocket();

    QUrl m_patternUrl;
    QUrl m_reportUrl;
    QUrl m_commandUrl;
    quint16 m_port    = 0;
    QString m_serverAddr;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QWebSocket* m_fleetSocket = nullptr;
    int m_fleetReconnectMs = 5000;

    std::function<void(int, const QString&)> m_welcomeCallback;
    std::function<QString(const QJsonObject&)> m_rpcDispatch;

    QMutex m_patternMutex;
    QList<QRegularExpression> m_patterns;

    static constexpr int FETCH_TIMEOUT_MS = 5000;
    static constexpr int FLEET_RECONNECT_MAX_MS = 60000;
};
