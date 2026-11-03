Goal: finish the compatibility matrix for Jamulus PR #3636 "Support TCP for protocol messages" (jamulussoftware/jamulus, head 99c66a4607a8136adfa7ae5fe2bdc0d64ce330e7) against docs/TCP.md "Enhancement for TCP support" steps 2-11. A previous session already built the harness, proved the method, and filled 7 cells; its report is reproduced below and you extend it. Rules: state only what a run shows, no hedge words, pin every result to a SHA and the exact command, everything on loopback with a LOCAL directory, nothing may contact public Jamulus directories (check with tcpdump on eth0 and the server logs). Never comment, review or open anything on github.com. Never push. Commit the final report as cloud-3636-compat/REPORT.md in a local clone of jamulussoftware/jamulus on branch cloud-3636-compat. Up to 4 Sonnet sub-agents for well-scoped work (e.g. one lab per sub-agent on disjoint port ranges), but run cells that install iptables rules sequentially.

== Setup (fresh Ubuntu 24.04 container, root) ==
apt-get update; apt-get install -y qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev-tools libjack-jackd2-dev jackd2 iproute2 tcpdump iptables
git clone https://github.com/jamulussoftware/jamulus.git /home/user/jamulus
cd /home/user/jamulus && git fetch origin pull/3636/head:pr3636
NEW = 99c66a4607a8136adfa7ae5fe2bdc0d64ce330e7 (pr3636). OLD = 4c0dbf23a1ccd0a3624906064a2e03292ba77494 (merge-base with main, verify with `git merge-base pr3636 origin/main`).
git worktree add /home/user/jamulus-old 4c0dbf23a1ccd0a3624906064a2e03292ba77494
git worktree add /home/user/jamulus-new pr3636
In each worktree: qmake "CONFIG+=headless" && make -j2 2>&1 | tee /home/user/build-<old|new>.log   (the log is parsed by build-driver.sh; keep it)
jackd -d dummy -r 48000 -p 128 &    (the headless client throws if no JACK server; XRun lines in its output are harmless)
ip link set lo mtu 1500
Facts verified last time: iptables `-f` matches 0 packets here because nf_conntrack reassembles before INPUT; use the length rule in run-cell.sh instead. CSocket adds a random offset to the client port, so run-cell.sh reads the real port with `ss -lunp`. The OLD tree has no default: case for unknown CLM ids, so CLM_TCP_OFFERED (id 1023, 15 bytes) is dropped silently by OLD clients.

== Harness (recreate these files in /home/user/harness, then `chmod +x *.sh`) ==
--- build-driver.sh ---
#!/bin/bash
# build-driver.sh TREE_DIR BUILD_LOG OUT TREE_DEFINE
set -e
TREE=$1; LOG=$2; OUT=$3; DEF=$4
CXXFLAGS=$(grep -m1 '^g++ -c' "$LOG" | sed 's/^g++ -c //' | sed -E 's/ -o release\/[^ ]+ src\/.*$//')
LIBS=$(grep -m1 '^g++ -Wl' "$LOG" | sed -E 's/.* release\/[^ ]+\.o //')
cd "$TREE"
OBJS=$(ls release/*.o | grep -v '/main\.o$')
g++ $CXXFLAGS -DTREE_NAME="\"$DEF\"" -D$DEF -o /home/user/harness/$OUT.o -c /home/user/harness/cldrv.cpp
g++ -o /home/user/harness/$OUT /home/user/harness/$OUT.o $OBJS $LIBS
echo "built /home/user/harness/$OUT from $TREE ($(git -C $TREE rev-parse HEAD))"
Build with: ./build-driver.sh /home/user/jamulus-new /home/user/build-new.log cldrv-new TREE_NEW ; ./build-driver.sh /home/user/jamulus-old /home/user/build-old.log cldrv-old TREE_OLD

--- cldrv.cpp ---
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

--- start-lab.sh ---
#!/bin/bash
# start-lab.sh TREE(old|new) DIRPORT TCPFLAG("" or --enabletcp) NSERVERS
# Starts one local directory and NSERVERS local servers registered with it, all on 127.0.0.1.
TREE=$1; DIRPORT=$2; TCPFLAG=$3; N=${4:-50}
BIN=/home/user/jamulus-$TREE/Jamulus
LOGS=/home/user/harness/logs/lab-$TREE-$DIRPORT; mkdir -p $LOGS
echo "lab: tree=$TREE sha=$(git -C /home/user/jamulus-$TREE rev-parse HEAD) dirport=$DIRPORT tcp='$TCPFLAG' servers=$N"
# directory: a server whose --directoryaddress is "localhost" acts as the directory (serverlist.cpp SetIsDirectory)
$BIN -s -n -p $DIRPORT -e localhost $TCPFLAG --serverinfo "LocalDirectory$DIRPORT;Directory City;0" -u 10 > $LOGS/directory.log 2>&1 &
echo $! > $LOGS/directory.pid
sleep 1
for i in $(seq 1 $N); do
  P=$((DIRPORT + i))
  NAME=$(printf 'Srv%02d-LongServerNm' $i)    # 20 chars = MAX_LEN_SERVER_NAME
  CITY=$(printf 'City%02d-LongCityName' $i)   # 20 chars = MAX_LEN_SERVER_CITY
  # the NEW-tree server also gets TCPFLAG so its CLM_CONN_CLIENTS_LIST path offers TCP; OLD ignores (flag absent)
  $BIN -s -n -p $P -e 127.0.0.1:$DIRPORT $TCPFLAG --serverinfo "$NAME;$CITY;$((i % 200))" -u 150 > $LOGS/server$i.log 2>&1 &
  echo $! >> $LOGS/servers.pid
done
sleep 4
echo "registered (directory log):"; grep -c 'registered\|Registered' $LOGS/directory.log; grep -i 'tcp' $LOGS/directory.log

--- stop-lab.sh ---
#!/bin/bash
pkill -f '/home/user/jamulus-(old|new)/Jamulus -s' ; sleep 1; pgrep -af 'Jamulus -s' | wc -l

--- run-cell.sh ---
#!/bin/bash
# run-cell.sh CELLNAME CLIENTTREE(old|new) MODE(serverlist|clientlist) TARGET(host:port) CLIENTPORT TIMEOUT_S DROP(yes|no)
# DROP=yes: drop every UDP datagram to CLIENTPORT whose IP total length exceeds 1500 bytes (what a 1500-MTU path
#           loses when IP fragments are dropped). iptables -f cannot be used on this host: nf_conntrack reassembles
#           fragments before INPUT (verified: rule matched 0 pkts while tcpdump showed fragments on lo).
set -u
CELL=$1; TREE=$2; MODE=$3; TARGET=$4; CP=$5; TMO=$6; DROP=$7; TCPFAIL=${8:-none}   # TCPFAIL: none|reject|drop (applies to TCP dport of TARGET)
OUT=/home/user/harness/logs/cells/$CELL; mkdir -p $OUT
TPORT=${TARGET##*:}
case $TCPFAIL in
  reject) TRULE="INPUT -i lo -p tcp --dport $TPORT --syn -j REJECT --reject-with tcp-reset"; iptables -I $TRULE;;
  drop)   TRULE="INPUT -i lo -p tcp --dport $TPORT --syn -j DROP"; iptables -I $TRULE;;
esac
[ "$TCPFAIL" != none ] && echo "tcp rule: iptables -I $TRULE" | tee -a $OUT/cmd.txt
tcpdump -i lo -n -tttt -l "port $TPORT or port $CP" > $OUT/tcpdump.txt 2>/dev/null &
TD=$!
sleep 0.5
echo "cell=$CELL client=$TREE ($(git -C /home/user/jamulus-$TREE rev-parse HEAD)) mode=$MODE target=$TARGET clientport=$CP drop=$DROP" | tee $OUT/cmd.txt
echo "cmd: /home/user/harness/cldrv-$TREE $MODE $TARGET $CP $TMO" | tee -a $OUT/cmd.txt
/home/user/harness/cldrv-$TREE $MODE $TARGET $CP $TMO 1500 > $OUT/client.log 2>&1 &
DRV=$!
sleep 0.7
REALPORT=$(ss -Hlunp | grep "pid=$DRV," | sed -E 's/.*:([0-9]+) .*/\1/' | head -1)
echo "client real UDP port: $REALPORT" | tee -a $OUT/cmd.txt
RULE="INPUT -i lo -p udp --dport $REALPORT -m length --length 1501:65535 -j DROP"
[ "$DROP" = yes ] && { iptables -I $RULE; echo "rule: iptables -I $RULE" | tee -a $OUT/cmd.txt; }
wait $DRV
sleep 0.5
kill $TD; wait $TD 2>/dev/null
[ "$DROP" = yes ] && { iptables -L INPUT -n -v | grep "dpt:$REALPORT" | tee -a $OUT/cmd.txt; iptables -D $RULE; }
[ "$TCPFAIL" != none ] && { iptables -L INPUT -n -v | grep "dpt:$TPORT" | tee -a $OUT/cmd.txt; iptables -D $TRULE; }
CP=$REALPORT
echo "--- client.log (events) ---"; grep -v '^\[.*QT: .*XRun' $OUT/client.log | grep 'EVENT\|TRANSPORT\|ACTION\|TIMER\|SEND\|END\|QT:' 
echo "--- tcpdump: UDP datagram sizes from target, TCP SYNs ---"
grep -E "UDP, length" $OUT/tcpdump.txt | grep "127.0.0.1.$TPORT > 127.0.0.1.$CP" | awk '{print $NF}' | sort -n | uniq -c | sort -k2 -n | tail -5
grep -E "Flags \[S\]" $OUT/tcpdump.txt | head -5

--- headless client ini (name 16 chars, city 20 chars, base64) ---
<client>
 <name_base64>Q2xpZW50MDEtU2l4dGVlbg==</name_base64>
 <city_base64>Q2xpZW50Q2l0eTAxLVR3ZW50eUM=</city_base64>
</client>
Start a connected headless client: /home/user/jamulus-<tree>/Jamulus -n -c 127.0.0.1:<serverport> -i /home/user/harness/ini/cN.ini --clientname cN -p <unique port>
Each one uses ~10% of a CPU on a 4-CPU host; 32 clients worked out to ~1760 B of CLM_CONN_CLIENTS_LIST (55 B per client measured), which exceeds 1500 B. Start them in batches of 8 with 2 s gaps and confirm `connected (N)` lines in the server log before measuring.

== Work to do ==
1. Rebuild everything above, rerun the two reference cells to confirm the harness reproduces the previous results (expected: `smoke2`-style NEW client + NEW dir with TCP + list lost gives `TRANSPORT CLM_SERVER_LIST via TCP` within a few ms; OLD client + same dir + list lost never gets the full list and re-requests every 2 s).
2. Server list rows still missing, each with and without the loss rule (run-cell DROP=yes/no), 50 servers, 9 s timeout:
   - OLD client + OLD directory: ./start-lab.sh old 31000 "" 50 ; ./run-cell.sh <cell> old serverlist 127.0.0.1:31000 41000 9 no|yes
   - NEW client + OLD directory: same lab, cldrv-new.
   - NEW client + NEW directory WITHOUT --enabletcp: ./start-lab.sh new 32000 "" 50 ; cldrv-new. Confirm no 15-byte CLM_TCP_OFFERED in tcpdump and no SYN.
   Record the RED/FULL datagram sizes of the OLD directory and of the NEW directory without TCP (expected equal to 1285/2708 for 51 entries).
3. CLM_CONN_CLIENTS_LIST matrix on one server with >= 32 connected headless clients (use clients from the same tree as the server), driver mode `clientlist`, 12 s timeout, with and without DROP, rows: OLD client + OLD server; OLD client + NEW server --enabletcp; NEW client + OLD server; NEW client + NEW server without --enabletcp; NEW client + NEW server --enabletcp list not lost; same with list lost. For the lost case verify steps 7d, 8, 9: TCP SYN to the server port, `TRANSPORT CLM_CONN_CLIENTS_LIST via TCP`, then disconnect one headless client and confirm the next ping reply triggers `SEND msgid=1014 ... mode=TCP_ONCE` immediately (CFM_TCP_RESULT path of step 9) with no UDP attempt first.
4. Step 3a: during a NEW+TCP lost-list cell, count CLM_SEND_EMPTY_MESSAGE datagrams from the directory to the 50 servers (tcpdump `src port 30000 and dst portrange 30001-30050`) after the UDP request vs after the TCP request; the doc says none after the TCP request.
5. Step 4 re-fetch path: in the NEW client after a TCP result, call CreateCLReqServerListMes again without InitPendingLists (add a `--refetch` option to cldrv that issues a second request 3 s after the first) and confirm the second request is `mode=TCP_ONCE` immediately.
6. Steps 10 and 11 were already confirmed (B1 refused, B2 dropped SYN); rerun once with the CLM_CONN_CLIENTS_LIST path (TCPFAIL=reject and =drop on the server port) and record.
7. Outbound check: run `tcpdump -i eth0 -n 'not (tcp port 443) and not (tcp port 80) and not arp'` for the whole duration of one lab start plus 60 s; report every packet; grep all server logs for anygenre, jamulus.app, jamulus.io.
8. REPORT.md: keep the structure below, fill every NOT RUN cell, keep the existing cells, put any doc-vs-run difference as the headline finding with a log excerpt, end with the line saying what was NOT covered. Commit on branch cloud-3636-compat with the harness and all cell logs.

== Previous REPORT.md (reproduce, then extend) ==
# Jamulus PR #3636 "Support TCP for protocol messages": behaviour vs docs/TCP.md and compatibility with current main

Status: PARTIAL. This session (Fable) built both trees, wrote the test harness, verified the loss-simulation
method, and ran the cells that need the most judgement (TCP fallback with the list lost, OLD-client
compatibility, doc steps 10 and 11). The remaining matrix rows are listed under "NOT RUN" at the end and are
handed to the next session with the exact commands. Every number below comes from a run in this container.

## Trees and builds

| Tree | SHA | What |
|---|---|---|
| NEW | `99c66a4607a8136adfa7ae5fe2bdc0d64ce330e7` | PR head, `git fetch origin pull/3636/head` |
| OLD | `4c0dbf23a1ccd0a3624906064a2e03292ba77494` | `git merge-base pr3636 origin/main` (origin/main was `8ba7afcc` at clone time) |

Host: Ubuntu 24.04.5, 4 CPUs, Qt 5.15.13 (apt `qtbase5-dev qt5-qmake qtbase5-dev-tools qttools5-dev-tools`),
`libjack-jackd2-dev` (required even for a headless build: `Jamulus.pro` errors without it), `jackd2` (a dummy
JACK server is required by the headless client: `src/sound/jack/sound.cpp:65` throws if `jack_client_open` fails).

Build commands (identical in both worktrees):

```bash
qmake "CONFIG+=headless"
make -j2
```

Both produce `./Jamulus` (3.12.5dev-nogit). The headless build includes the client code (`HEADLESS`, not
`SERVER_ONLY`), so both the server and the headless client used below are the real binaries of each tree.

## Source reading (NEW tree, line numbers at 99c66a46)

* Directory mode: a server becomes a directory when its own `--directoryaddress` is `localhost` or `127.0.0.1`
  (`src/serverlist.cpp:425`, `SetIsDirectory`). Any other value such as `127.0.0.1:30000` registers it as an
  ordinary server with that directory.
* `--enabletcp`: `src/main.cpp:289`, server-only option, sets `bEnableTcp`; `CServer` constructor calls
  `TcpServer.Start()` only when set; `CTcpServer::Start` listens with `QTcpServer` on the same port as UDP and sets
  `bTCPv4Available`. Log line on success: `- Jamulus-TCP: IPv4 server started on 0.0.0.0:<port>`.
* `CLM_TCP_OFFERED` (id 1023) is sent by the directory after `CLM_RED_SERVER_LIST` and `CLM_SERVER_LIST` on a UDP
  request (`src/serverlist.cpp:761`), by a server after `CLM_CONN_CLIENTS_LIST` on a UDP request
  (`src/server.h:412`), and by a server before `CLIENT_ID` on a new connection (`src/server.cpp:419`).
* Client handling: `CClient::OnCLTcpOfferedReceived` (`src/client.cpp:1272`) re-requests over TCP only when a
  pending UDP request for that host is recorded (`CFM_UDP_REQUEST`); `CClient::OnSendCLProtMessage`
  (`src/client.cpp:283`) opens a `QTcpSocket` with a 3000 ms connect timeout (`TCP_CONNECT_TIMEOUT_MS`), and on
  error or timeout removes the pending entry so the next request goes over UDP again. The 2 s re-request path
  (`CClient::CreateCLReqServerListMes`, `src/client.cpp:427`) does nothing while the entry is `CFM_TCP_REQUEST`.
  After a list arrives over TCP the client-side `CTcpConnection` closes the socket (`src/tcpconnection.cpp:181`).
* OLD tree: `CProtocol::ParseConnectionLessMessageBody` has no `default:` case; an unknown id such as 1023 is
  dropped without any log line (`src/protocol.cpp:983` in OLD).

## Harness (all files in `cloud-3636-compat/harness/`)

* `cldrv.cpp`: test-only driver linked against each tree's real object files (everything except `main.o`). It
  subclasses the real `CClient` and calls exactly what the Connect dialog calls: `CreateCLReqServerListMes` plus
  the dialog's 2 s re-request timer, and `CreateCLServerListPingMes` every 2.5 s plus
  `CreateCLServerListReqConnClientsListMes` when the reported client count differs from the listed count
  (same condition as `CConnectDlg::SetPingTimeAndNumClientsResult`). For NEW it also calls `InitPendingLists()`
  as `CClientDlg::ShowConnectionSetupDialog` does. It logs every CClient signal and, for NEW, the transport of
  each received list. Built by `build-driver.sh` into `cldrv-old` and `cldrv-new`. The OLD client rows therefore
  use the real OLD client code; no script stand-in was needed.
* `start-lab.sh TREE DIRPORT [--enabletcp] N`: starts one directory (`-s -n -p DIRPORT -e localhost`) and N
  servers (`-s -n -p DIRPORT+i -e 127.0.0.1:DIRPORT --serverinfo "<20-char name>;<20-char city>;<country>"`).
* `run-cell.sh`: runs one cell with `tcpdump -i lo` on the target and client ports, optional UDP loss rule,
  optional TCP refusal rule, and prints the client log, datagram sizes and TCP SYNs.

### Loss simulation method (task item 4)

1. `ip link set lo mtu 1500` succeeded and `tcpdump -i lo` then showed a 5000-byte UDP datagram leaving as one
   first fragment plus three `ip-proto-17` fragments (`fragtest.py`).
2. `iptables -I INPUT -i lo -p udp -f -j DROP` was accepted but matched 0 packets and the 5000-byte datagram
   still arrived: `nf_conntrack` is loaded (`/sys/module/nf_conntrack`) and reassembles fragments before INPUT.
3. Method used instead: `iptables -I INPUT -i lo -p udp --dport <client UDP port> -m length --length 1501:65535 -j DROP`.
   This discards exactly the datagrams that need fragmentation on a 1500-byte MTU path, scoped to the client under
   test. Verified with `fragtest.py`: 1400 bytes received, 1500 and 5000 bytes lost. No client code was patched.
   The client's real UDP port is read with `ss -lunp` because `CSocket` adds a random offset to the requested port
   (`NUM_SOCKET_PORTS_TO_TRY`, `src/socket.cpp:348`); the driver waits 1500 ms before its first request so the rule
   is in place.

## Directory test and list sizes (task item 2)

Lab: `./start-lab.sh new 30000 --enabletcp 50` (NEW directory with TCP on 127.0.0.1:30000, 50 NEW servers on
30001-30050, all with `--enabletcp`). Directory log: `- Jamulus-TCP: IPv4 server started on 0.0.0.0:30000`.
Server 1 log: `Server Registration Status update: Registered` to `127.0.0.1:30000`.

Measured on the wire (`tcpdump -i lo`, UDP payload bytes, cell `smoke2-new-tcp-drop`), list of 51 entries
(directory itself + 50 servers, names and cities 20 characters each):

| Message | UDP payload bytes |
|---|---|
| CLM_REQ_SERVER_LIST (client to directory) | 9 |
| CLM_RED_SERVER_LIST | 1285 |
| CLM_SERVER_LIST | 2708 (IP total 2736, needs 2 fragments at MTU 1500) |
| CLM_TCP_OFFERED | 15 |
| CLM_CONN_CLIENTS_LIST for 3 connected clients (16-char names, 20-char cities) | 165 (55 bytes per client, so 28 or more clients exceed 1500) |

OLD-directory and NEW-directory-without-TCP sizes: NOT RUN (same encoding code, not measured).

## Matrix: server list (CLM_SERVER_LIST), one directory with 51 entries

Columns: list over UDP / TCP opened / full list received / time from first request to full list / errors in logs.
Cell logs: `cloud-3636-compat/logs/cells/<cell>/{client.log,tcpdump.txt,cmd.txt}`.

| Row | UDP list arrived | TCP opened | Full list | Time | Errors | Cell |
|---|---|---|---|---|---|---|
| OLD client + OLD directory | NOT RUN | | | | | |
| OLD client + NEW directory with TCP, list not lost | yes (RED 1285 B + FULL 2708 B) | no (0 SYN) | yes, 51 entries | 1 ms | none; the 15 B CLM_TCP_OFFERED is ignored silently | `A1-old-cli_new-dir-tcp_nodrop` |
| OLD client + NEW directory with TCP, list lost | RED yes, FULL dropped (4 x 2736 B dropped by rule) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none | `A2-old-cli_new-dir-tcp_drop` |
| NEW client + OLD directory | NOT RUN | | | | | |
| NEW client + NEW directory without TCP | NOT RUN | | | | | |
| NEW client + NEW directory with TCP, list not lost | yes | no (0 SYN) | yes, 51 entries, via UDP | 1 ms | none | `smoke-new-tcp-nodrop` |
| NEW client + NEW directory with TCP, list lost | RED yes, FULL dropped (1 x 2736 B) | yes (1 SYN, 1.618 s mark) | yes, 51 entries, via TCP | 3 ms (1.616 to 1.619) | none | `smoke2-new-tcp-drop` |
| NEW client + NEW directory with TCP, list lost, TCP port refused (RST) | RED yes, FULL dropped | attempted every 2 s, refused (8 SYN rejected) | no | never | `- TCP connection error: "Connection refused"` each cycle | `B1-new-cli_new-dir-tcp_drop_tcp-reject` |
| NEW client + NEW directory with TCP, list lost, TCP SYN dropped | RED yes, FULL dropped | attempted, times out after 3.15 s | no | never | `- Jamulus-TCP: timeout connecting to "127.0.0.1:30000"` | `B2-new-cli_new-dir-tcp_drop_tcp-drop` |

Key excerpt, `smoke2-new-tcp-drop/client.log` (NEW client, list lost; the SEND line is logged inside the
TCP_OFFERED handler, hence its order):

```
[   1.616] ACTION CreateCLReqServerListMes (initial)
[   1.616] SEND msgid=1007 bytes=9 to 127.0.0.1:30000 mode=UDP
[   1.617] EVENT CLM_RED_SERVER_LIST received from 127.0.0.1:30000 entries=51
[   1.618] SEND msgid=1007 bytes=9 to 127.0.0.1:30000 mode=TCP_ONCE
[   1.618] EVENT CLM_TCP_OFFERED received from 127.0.0.1:30000 for msgid=1006 token=0
[   1.619] EVENT CLM_SERVER_LIST received (CClient signal) from 127.0.0.1:30000 entries=51
[   1.619] TRANSPORT CLM_SERVER_LIST via TCP entries=51 from 127.0.0.1:30000
```
iptables counter for that cell: `1  2736 DROP ... udp dpt:41057 length 1501:65535`.

Excerpt, `B2-new-cli_new-dir-tcp_drop_tcp-drop/client.log` (steps 10 and 11):

```
[   1.616] SEND msgid=1007 bytes=9 to 127.0.0.1:30000 mode=TCP_ONCE
[   1.616] EVENT CLM_TCP_OFFERED received from 127.0.0.1:30000 for msgid=1006 token=0
[   3.718] TIMER 2s re-request fires: ACTION CreateCLReqServerListMes      <- no SEND follows: ignored (step 11)
[   4.767] QT: - Jamulus-TCP: timeout connecting to "127.0.0.1:30000"      <- 3.15 s after connect (step 10)
[   5.780] TIMER 2s re-request fires: ACTION CreateCLReqServerListMes
[   5.780] SEND msgid=1007 bytes=9 to 127.0.0.1:30000 mode=UDP               <- back on UDP (step 10)
```

## Matrix: connected-clients list (CLM_CONN_CLIENTS_LIST)

Feasibility cell only (`C0-feas-new-cli_new-srv-tcp_clientlist_nodrop`): 3 NEW headless clients
(`Jamulus -n -c 127.0.0.1:30001 -i <ini with name_base64/city_base64> --clientname cN -p <port>`) connected to
NEW server 30001 (`--enabletcp`); server log `connected (1)..(3)`. Driver ping reply `numclients=3`, then
`CLM_CONN_CLIENTS_LIST entries=3 via UDP` (165 B) followed by `CLM_TCP_OFFERED for msgid=1013`, no TCP opened.
`ss -tn` showed three ESTABLISHED TCP connections from the headless clients to 127.0.0.1:30001: the
connected-mode long-lived TCP session of docs/TCP.md "TCP usage in Connected Mode" is opened by NEW clients.
Each headless client used about 10% of one CPU, so this 4-CPU host supports roughly 30-35 of them.
All six matrix rows with a large client list: NOT RUN.

## docs/TCP.md "Enhancement for TCP support" steps 2-11 vs the runs

| Step | Doc says | Run shows | Match |
|---|---|---|---|
| 2 | directory sends TCP_OFFERED(CLM_SERVER_LIST) after RED and full list, on UDP | tcpdump order 1285, 2708, 15 bytes in every cell | yes |
| 2a | old client ignores it | A1, A2: OLD client logs nothing, keeps UDP | yes |
| 2b, 2c | new client receives it after the UDP list and does not open TCP | smoke-new-tcp-nodrop: 0 SYN | yes |
| 2d | without the list, client opens TCP and re-requests | smoke2: SYN at 1.618 s, `SEND ... mode=TCP_ONCE` | yes |
| 3 | directory answers the TCP request with CLM_SERVER_LIST only | smoke2: list arrived via TCP; no RED list over TCP in client log | yes (3a, no CLM_SEND_EMPTY_MESSAGE after the TCP request, not separately verified) |
| 4 | client closes TCP, stops the 2 s timer | smoke2: no later `TIMER` line; code closes socket at `tcpconnection.cpp:181` | yes |
| 5, 6 | pings and client-list requests stay on UDP | C0: `mode=UDP` for 1002 and 1014 | yes |
| 7 | server sends TCP_OFFERED(CLM_CONN_CLIENTS_LIST) after the UDP list | C0: 165 B list then 15 B offer, `msgid=1013` | yes |
| 7c | list already received, no TCP | C0: 0 SYN | yes |
| 7d, 8, 9 | TCP re-request of client list | NOT RUN (needs >= 28 connected clients) | |
| 10 | TCP fails or times out, revert to UDP | B1: refused, next 2 s tick sends UDP; B2: timeout after 3.15 s, next tick (5.780 s) sends UDP | yes |
| 11 | 2 s timer ignored while TCP pending | B2: tick at 3.718 s produced no request | yes |

No mismatch between docs/TCP.md and the runs was found in the cells executed. Observation, not a mismatch: after
a TCP failure the reversion to UDP happens at the next 2 s tick, not immediately, so with a dropped SYN the cycle
is UDP request, offer, 3 s TCP timeout, UDP request at the following tick (4 s period in B2).

## Outbound registration check

`tcpdump -i eth0 -n 'not (tcp port 443) and not (tcp port 80) and not arp'` for 25 s while the 51-process lab
was running captured 2 packets: one DNS query for `http-intake.logs.us5.datadoghq.com` from the container's
own tooling and its answer. No packet to or from UDP/TCP 22124 and no Jamulus traffic left the host
(`logs/eth0-outbound.txt`). Server logs mention only `127.0.0.1:30000` as directory; the strings
`anygenre`, `jamulus.app`, `jamulus.io` do not occur in any server log.

## NOT covered

Not run in this session: OLD client + OLD directory; NEW client + OLD directory; NEW client + NEW directory
without `--enabletcp`; every CLM_CONN_CLIENTS_LIST row with a list over 1500 bytes (steps 7d, 8, 9, and step 9's
immediate TCP re-request on the next count change); the CFM_TCP_RESULT re-fetch path of step 4; step 3a (no
CLM_SEND_EMPTY_MESSAGE on a TCP request) by packet count; IPv6; the GUI Connect dialog itself (the driver calls
the same CClient methods but the dialog was not executed); connected-mode TCP behaviour beyond the observation
that the session connections open.
