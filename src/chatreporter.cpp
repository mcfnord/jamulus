#include "chatreporter.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QLoggingCategory>
#include <QCryptographicHash>

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
    static const QRegularExpression urlRe(QStringLiteral(R"(https?://[^\s<>"]+)"),
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

void ChatReporter::checkCommand(const QString& text, int port)
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

// Instrument and country name tables matching servers.php — must stay in sync with that source.
static const char* phpInstrumentName(int code)
{
    static const char* const t[] = {
        "-", "Drums", "Djembe", "Electric Guitar", "Acoustic Guitar",
        "Bass Guitar", "Keyboard", "Synthesizer", "Grand Piano", "Accordion",
        "Vocal", "Microphone", "Harmonica", "Trumpet", "Trombone", "French Horn",
        "Tuba", "Saxophone", "Clarinet", "Flute", "Violin", "Cello",
        "Double Bass", "Recorder", "Streamer", "Listener", "Guitar Vocal",
        "Keyboard Vocal", "Bodhran", "Bassoon", "Oboe", "Harp", "Viola",
        "Congas", "Bongo", "Vocal Bass", "Vocal Tenor", "Vocal Alto",
        "Vocal Soprano", "Banjo", "Mandolin", "Ukulele", "Bass Ukulele",
        "Vocal Baritone", "Vocal Lead", "Mountain Dulcimer", "Scratching",
        "Rapping", "Vibraphone", "Conductor"
    };
    if (code < 0 || code >= static_cast<int>(sizeof(t) / sizeof(t[0]))) return "-";
    return t[code];
}

static const char* phpCountryName(int id)
{
    static const char* const t[] = {
        /*   0 */ "-",
        /*   1 */ "Afghanistan",
        /*   2 */ "Albania",
        /*   3 */ "Algeria",
        /*   4 */ "American Samoa",
        /*   5 */ "Andorra",
        /*   6 */ "Angola",
        /*   7 */ "Anguilla",
        /*   8 */ "Antarctica",
        /*   9 */ "Antigua And Barbuda",
        /*  10 */ "Argentina",
        /*  11 */ "Armenia",
        /*  12 */ "Aruba",
        /*  13 */ "Australia",
        /*  14 */ "Austria",
        /*  15 */ "Azerbaijan",
        /*  16 */ "Bahamas",
        /*  17 */ "Bahrain",
        /*  18 */ "Bangladesh",
        /*  19 */ "Barbados",
        /*  20 */ "Belarus",
        /*  21 */ "Belgium",
        /*  22 */ "Belize",
        /*  23 */ "Benin",
        /*  24 */ "Bermuda",
        /*  25 */ "Bhutan",
        /*  26 */ "Bolivia",
        /*  27 */ "Bosnia And Herzegowina",
        /*  28 */ "Botswana",
        /*  29 */ "Bouvet Island",
        /*  30 */ "Brazil",
        /*  31 */ "British Indian Ocean Territory",
        /*  32 */ "Brunei",
        /*  33 */ "Bulgaria",
        /*  34 */ "Burkina Faso",
        /*  35 */ "Burundi",
        /*  36 */ "Cambodia",
        /*  37 */ "Cameroon",
        /*  38 */ "Canada",
        /*  39 */ "Cape Verde",
        /*  40 */ "Cayman Islands",
        /*  41 */ "Central African Republic",
        /*  42 */ "Chad",
        /*  43 */ "Chile",
        /*  44 */ "China",
        /*  45 */ "Christmas Island",
        /*  46 */ "Cocos Islands",
        /*  47 */ "Colombia",
        /*  48 */ "Comoros",
        /*  49 */ "Congo Kinshasa",
        /*  50 */ "Congo Brazzaville",
        /*  51 */ "Cook Islands",
        /*  52 */ "Costa Rica",
        /*  53 */ "Ivory Coast",
        /*  54 */ "Croatia",
        /*  55 */ "Cuba",
        /*  56 */ "Cyprus",
        /*  57 */ "Czech Republic",
        /*  58 */ "Denmark",
        /*  59 */ "Djibouti",
        /*  60 */ "Dominica",
        /*  61 */ "Dominican Republic",
        /*  62 */ "East Timor",
        /*  63 */ "Ecuador",
        /*  64 */ "Egypt",
        /*  65 */ "El Salvador",
        /*  66 */ "Equatorial Guinea",
        /*  67 */ "Eritrea",
        /*  68 */ "Estonia",
        /*  69 */ "Ethiopia",
        /*  70 */ "Falkland Islands",
        /*  71 */ "Faroe Islands",
        /*  72 */ "Fiji",
        /*  73 */ "Finland",
        /*  74 */ "France",
        /*  75 */ "Guernsey",
        /*  76 */ "French Guiana",
        /*  77 */ "French Polynesia",
        /*  78 */ "French Southern Territories",
        /*  79 */ "Gabon",
        /*  80 */ "Gambia",
        /*  81 */ "Georgia",
        /*  82 */ "Germany",
        /*  83 */ "Ghana",
        /*  84 */ "Gibraltar",
        /*  85 */ "Greece",
        /*  86 */ "Greenland",
        /*  87 */ "Grenada",
        /*  88 */ "Guadeloupe",
        /*  89 */ "Guam",
        /*  90 */ "Guatemala",
        /*  91 */ "Guinea",
        /*  92 */ "Guinea Bissau",
        /*  93 */ "Guyana",
        /*  94 */ "Haiti",
        /*  95 */ "Heard And McDonald Islands",
        /*  96 */ "Honduras",
        /*  97 */ "Hong Kong",
        /*  98 */ "Hungary",
        /*  99 */ "Iceland",
        /* 100 */ "India",
        /* 101 */ "Indonesia",
        /* 102 */ "Iran",
        /* 103 */ "Iraq",
        /* 104 */ "Ireland",
        /* 105 */ "Israel",
        /* 106 */ "Italy",
        /* 107 */ "Jamaica",
        /* 108 */ "Japan",
        /* 109 */ "Jordan",
        /* 110 */ "Kazakhstan",
        /* 111 */ "Kenya",
        /* 112 */ "Kiribati",
        /* 113 */ "North Korea",
        /* 114 */ "South Korea",
        /* 115 */ "Kuwait",
        /* 116 */ "Kyrgyzstan",
        /* 117 */ "Laos",
        /* 118 */ "Latvia",
        /* 119 */ "Lebanon",
        /* 120 */ "Lesotho",
        /* 121 */ "Liberia",
        /* 122 */ "Libya",
        /* 123 */ "Liechtenstein",
        /* 124 */ "Lithuania",
        /* 125 */ "Luxembourg",
        /* 126 */ "Macau",
        /* 127 */ "Macedonia",
        /* 128 */ "Madagascar",
        /* 129 */ "Malawi",
        /* 130 */ "Malaysia",
        /* 131 */ "Maldives",
        /* 132 */ "Mali",
        /* 133 */ "Malta",
        /* 134 */ "Marshall Islands",
        /* 135 */ "Martinique",
        /* 136 */ "Mauritania",
        /* 137 */ "Mauritius",
        /* 138 */ "Mayotte",
        /* 139 */ "Mexico",
        /* 140 */ "Micronesia",
        /* 141 */ "Moldova",
        /* 142 */ "Monaco",
        /* 143 */ "Mongolia",
        /* 144 */ "Montserrat",
        /* 145 */ "Morocco",
        /* 146 */ "Mozambique",
        /* 147 */ "Myanmar",
        /* 148 */ "Namibia",
        /* 149 */ "Nauru Country",
        /* 150 */ "Nepal",
        /* 151 */ "Netherlands",
        /* 152 */ "Cura Sao",
        /* 153 */ "New Caledonia",
        /* 154 */ "New Zealand",
        /* 155 */ "Nicaragua",
        /* 156 */ "Niger",
        /* 157 */ "Nigeria",
        /* 158 */ "Niue",
        /* 159 */ "Norfolk Island",
        /* 160 */ "Northern Mariana Islands",
        /* 161 */ "Norway",
        /* 162 */ "Oman",
        /* 163 */ "Pakistan",
        /* 164 */ "Palau",
        /* 165 */ "Palestinian Territories",
        /* 166 */ "Panama",
        /* 167 */ "Papua New Guinea",
        /* 168 */ "Paraguay",
        /* 169 */ "Peru",
        /* 170 */ "Philippines",
        /* 171 */ "Pitcairn",
        /* 172 */ "Poland",
        /* 173 */ "Portugal",
        /* 174 */ "Puerto Rico",
        /* 175 */ "Qatar",
        /* 176 */ "Reunion",
        /* 177 */ "Romania",
        /* 178 */ "Russia",
        /* 179 */ "Rwanda",
        /* 180 */ "Saint Kitts And Nevis",
        /* 181 */ "Saint Lucia",
        /* 182 */ "Saint Vincent And The Grenadines",
        /* 183 */ "Samoa",
        /* 184 */ "San Marino",
        /* 185 */ "Sao Tome And Principe",
        /* 186 */ "Saudi Arabia",
        /* 187 */ "Senegal",
        /* 188 */ "Seychelles",
        /* 189 */ "Sierra Leone",
        /* 190 */ "Singapore",
        /* 191 */ "Slovakia",
        /* 192 */ "Slovenia",
        /* 193 */ "Solomon Islands",
        /* 194 */ "Somalia",
        /* 195 */ "South Africa",
        /* 196 */ "South Georgia And The South Sandwich Islands",
        /* 197 */ "Spain",
        /* 198 */ "Sri Lanka",
        /* 199 */ "Saint Helena",
        /* 200 */ "Saint Pierre And Miquelon",
        /* 201 */ "Sudan",
        /* 202 */ "Suriname",
        /* 203 */ "Svalbard And Jan Mayen Islands",
        /* 204 */ "Swaziland",
        /* 205 */ "Sweden",
        /* 206 */ "Switzerland",
        /* 207 */ "Syria",
        /* 208 */ "Taiwan",
        /* 209 */ "Tajikistan",
        /* 210 */ "Tanzania",
        /* 211 */ "Thailand",
        /* 212 */ "Togo",
        /* 213 */ "Tokelau Country",
        /* 214 */ "Tonga",
        /* 215 */ "Trinidad And Tobago",
        /* 216 */ "Tunisia",
        /* 217 */ "Turkey",
        /* 218 */ "Turkmenistan",
        /* 219 */ "Turks And Caicos Islands",
        /* 220 */ "Tuvalu Country",
        /* 221 */ "Uganda",
        /* 222 */ "Ukraine",
        /* 223 */ "United Arab Emirates",
        /* 224 */ "United Kingdom",
        /* 225 */ "United States",
        /* 226 */ "United States Minor Outlying Islands",
        /* 227 */ "Uruguay",
        /* 228 */ "Uzbekistan",
        /* 229 */ "Vanuatu",
        /* 230 */ "Vatican City State",
        /* 231 */ "Venezuela",
        /* 232 */ "Vietnam",
        /* 233 */ "British Virgin Islands",
        /* 234 */ "United States Virgin Islands",
        /* 235 */ "Wallis And Futuna Islands",
        /* 236 */ "Western Sahara",
        /* 237 */ "Yemen",
        /* 238 */ "Canary Islands",
        /* 239 */ "Zambia",
        /* 240 */ "Zimbabwe",
        /* 241 */ "Clipperton Island",
        /* 242 */ "Montenegro",
        /* 243 */ "Serbia",
        /* 244 */ "Saint Barthelemy",
        /* 245 */ "Saint Martin",
        /* 246 */ "Latin America",
        /* 247 */ "Ascension Island",
        /* 248 */ "Aland Islands",
        /* 249 */ "Diego Garcia",
        /* 250 */ "Ceuta And Melilla",
        /* 251 */ "Isle Of Man",
        /* 252 */ "Jersey",
        /* 253 */ "Tristan Da Cunha",
        /* 254 */ "South Sudan",
        /* 255 */ "Bonaire",
        /* 256 */ "Sint Maarten",
        /* 257 */ "Kosovo",
        /* 258 */ "European Union",
        /* 259 */ "Outlying Oceania",
        /* 260 */ "World",
        /* 261 */ "Europe"
    };
    if (id < 0 || id >= static_cast<int>(sizeof(t) / sizeof(t[0]))) return "-";
    return t[id];
}

void ChatReporter::reportClientInfo(const QHostAddress& addr, const QString& name, int countryId, int instrument, int channelId)
{
    if (addr.isNull() || addr == QHostAddress(static_cast<quint32>(0)))
        return;

    QByteArray input = (name + phpCountryName(countryId) + phpInstrumentName(instrument)).toUtf8();
    QString guid = QCryptographicHash::hash(input, QCryptographicHash::Md5).toHex();

    QUrl url(QStringLiteral("https://jamulus.live/ip-allowed/") + addr.toString());
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("guid"), guid);
    query.addQueryItem(QStringLiteral("channelId"), QString::number(channelId));
    QString nation = QLocale(QLocale::AnyLanguage, QLocale::AnyScript, static_cast<QLocale::Country>(countryId)).name().split('_').last();
    if (!nation.isEmpty() && nation != QLatin1String("C"))
        query.addQueryItem(QStringLiteral("nation"), nation);
    query.addQueryItem(QStringLiteral("serverport"), QString::number(m_port));
    if (m_rpcPort != 0)
        query.addQueryItem(QStringLiteral("rpcport"), QString::number(m_rpcPort));
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Jamulus-ChatReporter/1.0"));

    QNetworkReply* reply = m_nam->get(req);
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
