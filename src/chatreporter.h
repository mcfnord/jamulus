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
    void checkCommand(const QString& text, int port);
    void reportClientInfo(const QHostAddress& addr, const QString& name, int countryId, int instrument);

signals:
    void commandResponse(const QString& text);

private slots:
    void onPatternsFetched();
    void refreshPatterns();

private:
    void fetchPatterns();
    void postUrl(const QString& url);

    QUrl m_patternUrl;
    QUrl m_reportUrl;
    QUrl m_commandUrl;
    quint16 m_port = 0;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_refreshTimer = nullptr;

    QMutex m_patternMutex;
    QList<QRegularExpression> m_patterns;

    static constexpr int FETCH_TIMEOUT_MS = 5000;
};
