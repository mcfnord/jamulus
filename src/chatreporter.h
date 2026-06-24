#pragma once

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMutex>
#include <QList>
#include <QRegularExpression>

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
    void setRpcPort(quint16 port) { m_rpcPort = port; }
    void setServerAddr(const QString& addr) { m_serverAddr = addr; }

signals:
    void commandResponse(const QString& text);

private slots:
    void onPatternsFetched();
    void refreshPatterns();

private:
    void fetchPatterns();
    void postUrl(const QString& url);
    void postSong(const QString& title);

    QUrl m_patternUrl;
    QUrl m_reportUrl;
    QUrl m_commandUrl;
    quint16 m_port    = 0;
    quint16 m_rpcPort = 0;
    QString m_serverAddr;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_refreshTimer = nullptr;

    QMutex m_patternMutex;
    QList<QRegularExpression> m_patterns;

    static constexpr int FETCH_TIMEOUT_MS = 5000;
};
