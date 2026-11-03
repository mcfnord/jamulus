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
