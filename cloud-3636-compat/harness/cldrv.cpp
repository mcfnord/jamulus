// Test-only driver: links the real CClient of one Jamulus tree and drives the
// same request path the Connect dialog uses (CClient::CreateCLReqServerListMes,
// CClient::CreateCLServerListPingMes, CClient::CreateCLServerListReqConnClientsListMes),
// including the dialog's 2 s re-request timer and 2.5 s ping timer.
// Usage: cldrv serverlist HOST:PORT CLIENTPORT TIMEOUT_S [DELAY_MS]
//        cldrv clientlist HOST:PORT CLIENTPORT TIMEOUT_S [DELAY_MS]
// CLIENTPORT is the base port given to CClient; CSocket adds a random offset (NUM_SOCKET_PORTS_TO_TRY),
// so the harness reads the real port from ss during DELAY_MS before the first request is sent.
#include <QCoreApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <cstdio>
#include <unistd.h>
#include "client.h"
#include "global.h"
#define SERV_LIST_REQ_UPDATE_TIME_MS 2000 // same value as src/connectdlg.h (GUI header, not included in headless objects)

static QElapsedTimer g_clock;
static void logf ( const char* fmt, ... )
{
    va_list ap;
    va_start ( ap, fmt );
    fprintf ( stdout, "[%8.3f] ", g_clock.nsecsElapsed() / 1e9 );
    vfprintf ( stdout, fmt, ap );
    fprintf ( stdout, "\n" );
    fflush ( stdout );
    va_end ( ap );
}
static void qtHandler ( QtMsgType, const QMessageLogContext&, const QString& msg ) { logf ( "QT: %s", qUtf8Printable ( msg ) ); }

class CDrv : public CClient
{
public:
    CDrv ( quint16 port ) : CClient ( port, 0, true, "cldrv", true, false ) {}
    CProtocol& CLP() { return ConnLessProtocol; }
};

int main ( int argc, char** argv )
{
    QCoreApplication app ( argc, argv );
    if ( argc < 5 )
    {
        fprintf ( stderr, "usage: %s serverlist|clientlist HOST:PORT CLIENTPORT TIMEOUT_S\n", argv[0] );
        return 2;
    }
    QString      mode    = argv[1];
    QStringList  hp      = QString ( argv[2] ).split ( ':' );
    quint16      cliport = QString ( argv[3] ).toUShort();
    int          tmo     = QString ( argv[4] ).toInt();
    int          delay   = argc > 5 ? QString ( argv[5] ).toInt() : 0;
    CHostAddress target ( QHostAddress ( hp[0] ), hp[1].toUShort() );
    g_clock.start();
    qInstallMessageHandler ( qtHandler );

    CDrv cl ( cliport );
    logf ( "driver started mode=%s target=%s clientport=%u tree=%s", qUtf8Printable ( mode ), qUtf8Printable ( target.toString() ), cliport, TREE_NAME );

    bool bFull = false, bRed = false;
    int  iLastNumClients = -1, iListedClients = -1, iNumFull = 0;

    // --- signals as seen by the connect dialog (CClient level) ---
    QObject::connect ( &cl, &CClient::CLRedServerListReceived, [&] ( CHostAddress a, CVector<CServerInfo> v ) {
        bRed = true;
        logf ( "EVENT CLM_RED_SERVER_LIST received from %s entries=%d", qUtf8Printable ( a.toString() ), v.Size() );
    } );
    QObject::connect ( &cl, &CClient::CLServerListReceived, [&] ( CHostAddress a, CVector<CServerInfo> v ) {
        bFull = true;
        iNumFull++;
        logf ( "EVENT CLM_SERVER_LIST received (CClient signal) from %s entries=%d", qUtf8Printable ( a.toString() ), v.Size() );
    } );
    QObject::connect ( &cl, &CClient::CLConnClientsListMesReceived, [&] ( CHostAddress a, CVector<CChannelInfo> v ) {
        iListedClients = v.Size();
        logf ( "EVENT CLM_CONN_CLIENTS_LIST received (CClient signal) from %s entries=%d", qUtf8Printable ( a.toString() ), v.Size() );
    } );
    QObject::connect ( &cl, &CClient::CLPingTimeWithNumClientsReceived, [&] ( CHostAddress a, int ms, int n ) {
        logf ( "EVENT CLM_PING_MS_WITHNUMCLIENTS reply from %s ping=%dms numclients=%d", qUtf8Printable ( a.toString() ), ms, n );
        if ( mode == "clientlist" && n != iListedClients )
        {
            // same condition as CConnectDlg::SetPingTimeAndNumClientsResult
            logf ( "ACTION CreateCLServerListReqConnClientsListMes (listed=%d, reported=%d)", iListedClients, n );
            cl.CreateCLServerListReqConnClientsListMes ( a );
        }
        iLastNumClients = n;
    } );
#ifdef TREE_NEW
    // transport-level view (protocol signal carries the TCP connection pointer)
    QObject::connect ( &cl.CLP(), &CProtocol::CLServerListReceived, [&] ( CHostAddress a, CVector<CServerInfo> v, CTcpConnection* t ) {
        logf ( "TRANSPORT CLM_SERVER_LIST via %s entries=%d from %s", t ? "TCP" : "UDP", v.Size(), qUtf8Printable ( a.toString() ) );
    } );
    QObject::connect ( &cl.CLP(), &CProtocol::CLConnClientsListMesReceived, [&] ( CHostAddress a, CVector<CChannelInfo> v, CTcpConnection* t ) {
        logf ( "TRANSPORT CLM_CONN_CLIENTS_LIST via %s entries=%d from %s", t ? "TCP" : "UDP", v.Size(), qUtf8Printable ( a.toString() ) );
    } );
    QObject::connect ( &cl.CLP(), &CProtocol::CLTcpOfferedReceived, [&] ( CHostAddress a, int id, quint32 tok ) {
        logf ( "EVENT CLM_TCP_OFFERED received from %s for msgid=%d token=%u", qUtf8Printable ( a.toString() ), id, tok );
    } );
    QObject::connect ( &cl.CLP(), &CProtocol::CLMessReadyForSending, [&] ( int id, CHostAddress a, CVector<uint8_t> v, CTcpConnection* t, enum EProtoMode m ) {
        logf ( "SEND msgid=%d bytes=%d to %s mode=%s", id, v.Size(), qUtf8Printable ( a.toString() ), t ? "existing-TCP" : ( m == PROTO_UDP ? "UDP" : ( m == PROTO_TCP_ONCE ? "TCP_ONCE" : "TCP_LONG" ) ) );
    } );
    cl.InitPendingLists(); // CClientDlg::ShowConnectionSetupDialog does this before showing the dialog
#else
    QObject::connect ( &cl.CLP(), &CProtocol::CLMessReadyForSending, [&] ( CHostAddress a, CVector<uint8_t> v ) {
        logf ( "SEND bytes=%d to %s mode=UDP", v.Size(), qUtf8Printable ( a.toString() ) );
    } );
#endif

    QTimer tReq, tPing, tEnd, tStart;
    auto start = [&]() {
    if ( mode == "serverlist" )
    {
        // CConnectDlg::RequestServerList + TimerReRequestServList (SERV_LIST_REQ_UPDATE_TIME_MS)
        logf ( "ACTION CreateCLReqServerListMes (initial)" );
        cl.CreateCLReqServerListMes ( target );
        QObject::connect ( &tReq, &QTimer::timeout, [&]() {
            if ( !bFull )
            {
                logf ( "TIMER 2s re-request fires: ACTION CreateCLReqServerListMes" );
                cl.CreateCLReqServerListMes ( target );
            }
        } );
        tReq.start ( SERV_LIST_REQ_UPDATE_TIME_MS );
    }
    else
    {
        // CConnectDlg::OnTimerPing (PING_UPDATE_TIME_SERVER_LIST_MS)
        auto ping = [&]() {
            logf ( "ACTION CreateCLServerListPingMes" );
            cl.CreateCLServerListPingMes ( target );
        };
        ping();
        QObject::connect ( &tPing, &QTimer::timeout, ping );
        tPing.start ( PING_UPDATE_TIME_SERVER_LIST_MS );
    }
    };
    tStart.setSingleShot ( true );
    QObject::connect ( &tStart, &QTimer::timeout, start );
    logf ( "driver pid=%d waiting %d ms before first request", (int) getpid(), delay );
    tStart.start ( delay );
    QObject::connect ( &tEnd, &QTimer::timeout, [&]() {
        logf ( "END full_server_list=%s red_list=%s full_count=%d listed_clients=%d last_numclients=%d", bFull ? "yes" : "no", bRed ? "yes" : "no", iNumFull, iListedClients, iLastNumClients );
        app.quit();
    } );
    tEnd.setSingleShot ( true );
    tEnd.start ( tmo * 1000 );
    return app.exec();
}
