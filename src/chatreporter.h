#pragma once

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMutex>
#include <QList>
#include <QRegularExpression>

class ChatReporter : public QObject
{
    Q_OBJECT

public:
    explicit ChatReporter(const QUrl& patternUrl, const QUrl& reportUrl, QObject* parent = nullptr);

    void start();
    void reportIfMatch(const QString& text);

private slots:
    void onPatternsFetched();
    void refreshPatterns();

private:
    void fetchPatterns();
    void postUrl(const QString& url);

    QUrl m_patternUrl;
    QUrl m_reportUrl;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_refreshTimer = nullptr;

    QMutex m_patternMutex;
    QList<QRegularExpression> m_patterns;

    static constexpr int FETCH_TIMEOUT_MS = 5000;
};
