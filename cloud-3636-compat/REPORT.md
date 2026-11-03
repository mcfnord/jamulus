# Jamulus PR #3636 "Support TCP for protocol messages": behaviour vs docs/TCP.md and compatibility with current main

Status: COMPLETE for the requested matrix. The first session built both trees, wrote the harness, verified the
loss-simulation method and ran the A, B, C0 and smoke cells. The second session reran two reference cells
(R1, R2: same result as smoke2 and A2), then ran every row that session 1 left open: cells D to J and R3/R4.
Every number below comes from a run in this container, on loopback only, with a local directory.

## Headline finding

**No difference between docs/TCP.md "Enhancement for TCP support" steps 2-11 and the runs.** Every step now has
a cell. The three steps that were open before all behave as written:

* Step 3a: the directory sent 50 CLM_SEND_EMPTY_MESSAGE datagrams after the UDP request and 0 after the TCP
  request (cell `F-step3a-new-cli_new-dir-tcp-publicsrv_drop`, tcpdump on lo):
  ```
  22:24:04.660849 IP 198.51.100.10.33000 > 198.51.100.10.33002: UDP, length 15   <- first of 50 empty-message requests
  22:24:04.662326 IP 198.51.100.10.33000 > 198.51.100.10.33050: UDP, length 15   <- last of 50
  22:24:04.662468 127.0.0.1.33000 > 127.0.0.1.41769: UDP, length 1285            <- CLM_RED_SERVER_LIST
  22:24:04.662749 127.0.0.1.33000 > 127.0.0.1.41769: UDP, length 2708            <- CLM_SERVER_LIST (dropped by rule)
  22:24:04.663710 127.0.0.1.34304 > 127.0.0.1.33000: Flags [S]                   <- TCP re-request; no empty message follows
  ```
* Steps 7d, 8, 9: a 1777-byte CLM_CONN_CLIENTS_LIST (34 clients) was lost over UDP, refetched over TCP, and after
  one client disconnected the next ping reply sent the request as TCP_ONCE with no UDP attempt
  (cell `H-new-cli_new-srv-tcp_clientlist_drop_disconnect`):
  ```
  [   1.484] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=UDP
  [   1.485] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=TCP_ONCE
  [   1.485] EVENT CLM_TCP_OFFERED received from 127.0.0.1:35000 for msgid=1013 token=0
  [   1.487] TRANSPORT CLM_CONN_CLIENTS_LIST via TCP entries=34 from 127.0.0.1:35000
  [   6.442] EVENT CLM_PING_MS_WITHNUMCLIENTS reply from 127.0.0.1:35000 ping=0ms numclients=33
  [   6.442] ACTION CreateCLServerListReqConnClientsListMes (listed=34, reported=33)
  [   6.442] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=TCP_ONCE
  [   6.445] TRANSPORT CLM_CONN_CLIENTS_LIST via TCP entries=33 from 127.0.0.1:35000
  ```
* Step 4 re-fetch: a second CreateCLReqServerListMes 3 s after a TCP result, without InitPendingLists, went out
  as TCP_ONCE at once (cell `R3-new-cli_new-dir-tcp_drop_refetch`):
  ```
  [   1.613] TRANSPORT CLM_SERVER_LIST via TCP entries=51 from 127.0.0.1:30000
  [   4.639] REFETCH 3 s after first request: ACTION CreateCLReqServerListMes (no InitPendingLists)
  [   4.640] SEND msgid=1007 bytes=9 to 127.0.0.1:30000 mode=TCP_ONCE
  [   4.641] TRANSPORT CLM_SERVER_LIST via TCP entries=51 from 127.0.0.1:30000
  ```

Observations that are not doc mismatches:

1. **Two SYNs per refused request.** With the TCP port refused, each TCP_ONCE request produced two SYNs. strace
   of the driver (cell `I2-strace-...`) shows two `connect()` calls on the same fd within 1 ms, both
   `ECONNREFUSED`, for one `connectToHost` call (`src/client.cpp:384`) and one `TCP connection error` log line.
   The retry happens inside Qt 5.15.13, not in Jamulus code. The earlier B1 cell (8 SYN in 4 cycles) is the same effect.
2. **No TCP fallback against an OLD server or directory, or a NEW one without `--enabletcp`.** A NEW client never
   gets a lost list from those peers (cells D, E, G, J with loss). That is the OLD behaviour, and the doc only
   promises TCP when the peer offers it.
3. **Connected-mode sessions depend on `--enabletcp`.** 34 NEW headless clients held 34 ESTABLISHED TCP
   connections to a NEW server with `--enabletcp` and 0 to a NEW server without it (`ss -Htn state established`).

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
  optional TCP refusal rule, and prints the client log, datagram sizes and TCP SYNs. Added in session 2: `EXTRA`
  (extra driver arguments such as `--refetch`), `DURING` (background command while the driver runs, used to
  disconnect a client), `STRACE=1` (trace the driver's `connect()` calls), and for `clientlist` mode a capture
  filter limited to the driver's UDP port range plus TCP on the target port, because the target port also carries
  the audio of the connected clients. The four G cells were captured before that change and their
  `tcpdump.txt` was trimmed afterwards to the driver's real port and TCP flags (`tcpdump-filter.txt` says so).
* `cldrv --refetch` (session 2): in `serverlist` mode, issues a second `CreateCLReqServerListMes` 3 s after the
  first, with no `InitPendingLists` in between.
* `start-lab.sh` (session 2): `DIRHOST=<addr>` makes the servers register to `<addr>:DIRPORT` instead of
  `127.0.0.1:DIRPORT` (used only for step 3a).
* `start-clsrv.sh TREE PORT [--enabletcp] N` / `stop-clsrv.sh` (session 2): one server with no
  `--directoryaddress` (it registers nowhere; its log has no `register with` line) plus N headless clients of the
  same tree, in batches of 8 with 2 s gaps. Client i uses `ini34/c<i>.ini` (16-character name
  `ClientNN-Sixteen`, 20-character city `ClientCityNN-TwentyC`) and base port 50000+100*i.

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

Same list from the other directories (session 2, tcpdump on lo, datagrams from the directory to the client):

| Directory | Cell | CLM_RED_SERVER_LIST | CLM_SERVER_LIST | 15 B CLM_TCP_OFFERED |
|---|---|---|---|---|
| OLD `4c0dbf23`, `./start-lab.sh old 31000 "" 50` | `D-old-cli_old-dir_nodrop` | 1285 | 2708 | none |
| NEW `99c66a46` without `--enabletcp`, `./start-lab.sh new 32000 "" 50` | `E-new-cli_new-dir-notcp_nodrop` | 1285 | 2708 | none |
| NEW `99c66a46` with `--enabletcp` (reference) | `R1-new-cli_new-dir-tcp_drop` | 1285 | 2708 | 1 |

CLM_CONN_CLIENTS_LIST for 34 connected clients is 1777 bytes of UDP payload (1805 bytes IP total, the iptables
counter value) from the OLD server and from both NEW servers.

## Matrix: server list (CLM_SERVER_LIST), one directory with 51 entries

Columns: list over UDP / TCP opened / full list received / time from first request to full list / errors in logs.
Cell logs: `cloud-3636-compat/logs/cells/<cell>/{client.log,tcpdump.txt,cmd.txt}`.

| Row | UDP list arrived | TCP opened | Full list | Time | Errors | Cell |
|---|---|---|---|---|---|---|
| OLD client + OLD directory, list not lost | yes (RED 1285 B + FULL 2708 B) | no (0 SYN) | yes, 51 entries | 2 ms (1.608 to 1.610) | none | `D-old-cli_old-dir_nodrop` |
| OLD client + OLD directory, list lost | RED yes, FULL dropped (4 x 2736 B) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none | `D-old-cli_old-dir_drop` |
| OLD client + NEW directory with TCP, list not lost | yes (RED 1285 B + FULL 2708 B) | no (0 SYN) | yes, 51 entries | 1 ms | none; the 15 B CLM_TCP_OFFERED is ignored silently | `A1-old-cli_new-dir-tcp_nodrop` |
| OLD client + NEW directory with TCP, list lost | RED yes, FULL dropped (4 x 2736 B dropped by rule) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none | `A2-old-cli_new-dir-tcp_drop` |
| NEW client + OLD directory, list not lost | yes, via UDP | no (0 SYN) | yes, 51 entries | 1 ms (1.460 to 1.461) | none; no offer sent | `D-new-cli_old-dir_nodrop` |
| NEW client + OLD directory, list lost | RED yes, FULL dropped (4 x 2736 B) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none | `D-new-cli_old-dir_drop` |
| NEW client + NEW directory without TCP, list not lost | yes, via UDP | no (0 SYN) | yes, 51 entries | 1 ms (1.610 to 1.611) | none; 0 datagrams of 15 B | `E-new-cli_new-dir-notcp_nodrop` |
| NEW client + NEW directory without TCP, list lost | RED yes, FULL dropped (4 x 2736 B) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none; 0 datagrams of 15 B | `E-new-cli_new-dir-notcp_drop` |
| NEW client + NEW directory with TCP, list not lost | yes | no (0 SYN) | yes, 51 entries, via UDP | 1 ms | none | `smoke-new-tcp-nodrop` |
| NEW client + NEW directory with TCP, list lost | RED yes, FULL dropped (1 x 2736 B) | yes (1 SYN, 1.618 s mark) | yes, 51 entries, via TCP | 3 ms (1.616 to 1.619) | none | `smoke2-new-tcp-drop` |
| NEW client + NEW directory with TCP, list lost, TCP port refused (RST) | RED yes, FULL dropped | attempted every 2 s, refused (8 SYN rejected) | no | never | `- TCP connection error: "Connection refused"` each cycle | `B1-new-cli_new-dir-tcp_drop_tcp-reject` |
| NEW client + NEW directory with TCP, list lost, TCP SYN dropped | RED yes, FULL dropped | attempted, times out after 3.15 s | no | never | `- Jamulus-TCP: timeout connecting to "127.0.0.1:30000"` | `B2-new-cli_new-dir-tcp_drop_tcp-drop` |
| Reference rerun (session 2): NEW client + NEW directory with TCP, list lost | RED yes, FULL dropped (1 x 2736 B) | yes (1 SYN) | yes, 51 entries, via TCP | 3 ms (1.580 to 1.583) | none | `R1-new-cli_new-dir-tcp_drop` |
| Reference rerun (session 2): OLD client + NEW directory with TCP, list lost | RED yes, FULL dropped (4 x 2736 B) | no (0 SYN) | no; re-request every 2 s, 4 attempts in 9 s | never | none | `R2-old-cli_new-dir-tcp_drop` |
| Step 4 re-fetch: NEW client + NEW directory with TCP, list lost, `--refetch` | first request as R1; second request at 4.640 s | yes (2 SYN: 1.612 s and 4.640 s) | yes twice, both via TCP | second fetch 1 ms (4.640 to 4.641) | none | `R3-new-cli_new-dir-tcp_drop_refetch` |
| Step 4 control: NEW client + NEW directory with TCP, list not lost, `--refetch` | yes both times | no (0 SYN) | yes twice, both via UDP | 1 ms each | none | `R4-new-cli_new-dir-tcp_nodrop_refetch` |

Commands for session 2 server-list cells (client trees at the SHAs above, DROP is the last argument):

```bash
./start-lab.sh new 30000 --enabletcp 50
./run-cell.sh R1-new-cli_new-dir-tcp_drop new serverlist 127.0.0.1:30000 41000 9 yes
./run-cell.sh R2-old-cli_new-dir-tcp_drop old serverlist 127.0.0.1:30000 41100 9 yes
EXTRA=--refetch ./run-cell.sh R3-new-cli_new-dir-tcp_drop_refetch new serverlist 127.0.0.1:30000 41200 9 yes
EXTRA=--refetch ./run-cell.sh R4-new-cli_new-dir-tcp_nodrop_refetch new serverlist 127.0.0.1:30000 41300 9 no
```
```bash
./start-lab.sh old 31000 "" 50
./run-cell.sh D-old-cli_old-dir_nodrop old serverlist 127.0.0.1:31000 41400 9 no
./run-cell.sh D-old-cli_old-dir_drop old serverlist 127.0.0.1:31000 41400 9 yes
./run-cell.sh D-new-cli_old-dir_nodrop new serverlist 127.0.0.1:31000 41500 9 no
./run-cell.sh D-new-cli_old-dir_drop new serverlist 127.0.0.1:31000 41500 9 yes
```
```bash
./start-lab.sh new 32000 "" 50
./run-cell.sh E-new-cli_new-dir-notcp_nodrop new serverlist 127.0.0.1:32000 41600 9 no
./run-cell.sh E-new-cli_new-dir-notcp_drop new serverlist 127.0.0.1:32000 41600 9 yes
```

### Step 3a (CLM_SEND_EMPTY_MESSAGE) by packet count

On the 127.0.0.1 lab the directory sends no CLM_SEND_EMPTY_MESSAGE at all, after any request: `RetrieveAll`
(`src/serverlist.cpp:737`) skips servers whose address `NetworkUtil::IsPrivateNetworkIP` accepts, and that list
includes 127.0.0.0/8 (`src/util.cpp:1127`). A capture during R1 (`logs/R1-empty-127.txt`, filter
`udp and src port 30000 and dst portrange 30001-30050`) holds 0 packets. To make step 3a observable, a
documentation-range address was added to lo only for this lab and removed afterwards:

```bash
ip addr add 198.51.100.10/32 dev lo
DIRHOST=198.51.100.10 ./start-lab.sh new 33000 --enabletcp 50
tcpdump -i lo -n -tttt 'udp and src port 33000 and dst portrange 33001-33050'   # per cell, empty-msgs.txt
./run-cell.sh F-step3a-new-cli_new-dir-tcp-publicsrv_nodrop new serverlist 127.0.0.1:33000 41700 9 no
./run-cell.sh F-step3a-new-cli_new-dir-tcp-publicsrv_drop new serverlist 127.0.0.1:33000 41700 9 yes
ip addr del 198.51.100.10/32 dev lo
```

All 50 servers logged `register with directory at address: 198.51.100.10:33000` and `Registered`.

| Cell | Requests to directory | 15 B datagrams directory to servers | Last one vs TCP SYN |
|---|---|---|---|
| `F-step3a-..._nodrop` | 1 UDP | 50 | no SYN |
| `F-step3a-..._drop` | 1 UDP + 1 TCP | 50 | last at 22:24:04.662326, SYN at 22:24:04.663710, none after |

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

Feasibility cell from session 1 (`C0-feas-new-cli_new-srv-tcp_clientlist_nodrop`): 3 NEW headless clients
(`Jamulus -n -c 127.0.0.1:30001 -i <ini with name_base64/city_base64> --clientname cN -p <port>`) connected to
NEW server 30001 (`--enabletcp`); server log `connected (1)..(3)`. Driver ping reply `numclients=3`, then
`CLM_CONN_CLIENTS_LIST entries=3 via UDP` (165 B) followed by `CLM_TCP_OFFERED for msgid=1013`, no TCP opened.
`ss -tn` showed three ESTABLISHED TCP connections from the headless clients to 127.0.0.1:30001: the
connected-mode long-lived TCP session of docs/TCP.md "TCP usage in Connected Mode" is opened by NEW clients.
Each headless client used about 10% of one CPU.

Session 2 matrix: one server with 34 headless clients of the same tree as the server, all confirmed by
`connected (34)` in the server log. List size 1777 B UDP payload (1805 B IP total). Driver mode `clientlist`,
12 s timeout. Columns as above; "requests" counts CLM_REQ_CONN_CLIENTS_LIST (msgid 1014) sent by the driver.

| Row | UDP list arrived | TCP opened | Full list | Time | Errors | Cell |
|---|---|---|---|---|---|---|
| OLD client + OLD server, not lost | yes, 1777 B | no (0 SYN) | yes, 34 entries | 1 ms (1.621 to 1.622) | none; no offer | `G-old-cli_old-srv_clientlist_nodrop` |
| OLD client + OLD server, lost | dropped (4 x 1805 B) | no (0 SYN) | no; re-request on every ping, 4 requests | never | none | `G-old-cli_old-srv_clientlist_drop` |
| OLD client + NEW server `--enabletcp`, not lost | yes, 1777 B, then 15 B offer | no (0 SYN) | yes, 34 entries | 1 ms (1.479 to 1.480) | none; offer ignored silently | `H-old-cli_new-srv-tcp_clientlist_nodrop` |
| OLD client + NEW server `--enabletcp`, lost | dropped (4 x 1805 B), 4 offers arrived | no (0 SYN) | no; re-request on every ping, 4 requests | never | none | `H-old-cli_new-srv-tcp_clientlist_drop` |
| NEW client + OLD server, not lost | yes, via UDP | no (0 SYN) | yes, 34 entries | 1 ms (1.562 to 1.563) | none; no offer | `G-new-cli_old-srv_clientlist_nodrop` |
| NEW client + OLD server, lost | dropped (4 x 1805 B), no offer | no (0 SYN) | no; re-request on every ping, 4 requests | never | none | `G-new-cli_old-srv_clientlist_drop` |
| NEW client + NEW server without TCP, not lost | yes, via UDP | no (0 SYN) | yes, 34 entries | 2 ms (1.481 to 1.483) | none; no offer | `J-new-cli_new-srv-notcp_clientlist_nodrop` |
| NEW client + NEW server without TCP, lost | dropped (5 x 1805 B), no offer | no (0 SYN) | no; re-request on every ping, 5 requests | never | none | `J-new-cli_new-srv-notcp_clientlist_drop` |
| NEW client + NEW server `--enabletcp`, not lost | yes, via UDP, then offer `msgid=1013` | no (0 SYN) | yes, 34 entries | 1 ms (1.646 to 1.647) | none | `H-new-cli_new-srv-tcp_clientlist_nodrop` |
| NEW client + NEW server `--enabletcp`, lost, client 34 disconnected at about 6 s | dropped (1 x 1805 B), offer arrived | yes (1.485 s), and again at 6.442 s after the count change | yes, 34 entries via TCP, then 33 entries via TCP | 3 ms (1.484 to 1.487); 3 ms (6.442 to 6.445) | none | `H-new-cli_new-srv-tcp_clientlist_drop_disconnect` |
| NEW client + NEW server `--enabletcp`, lost, TCP port refused (RST) | dropped, offer arrived each cycle | attempted each ping cycle, refused (10 SYN, 10 RST in 5 cycles) | no | never | `- TCP connection error: "Connection refused"` 5 times | `I-new-cli_new-srv-tcp_clientlist_drop_tcp-reject` |
| NEW client + NEW server `--enabletcp`, lost, TCP SYN dropped | dropped, offer arrived | attempted twice, timed out 3.13 s and 3.00 s after each TCP_ONCE (7 SYN incl. kernel retransmits) | no | never | `- Jamulus-TCP: timeout connecting to "127.0.0.1:35000"` 2 times | `I-new-cli_new-srv-tcp_clientlist_drop_tcp-drop` |
| Same as refused row, under strace | dropped | 2 `connect()` per request, both ECONNREFUSED | no | never | 3 refusals in 7 s | `I2-strace-new-cli_new-srv-tcp_clientlist_drop_tcp-reject` |

In the disconnect cell the driver sent msgid 1014 over UDP exactly once (1.484 s). After the TCP result, no
UDP 1014 was sent. The server's 1777 B datagram to the driver appears once in tcpdump, and the iptables counter
reads `1  1805 DROP ... udp dpt:42496 length 1501:65535`. The disconnect was `kill <pid of client 34>` at
22:28:07.945 UTC (`during.log`); the ping reply at 6.442 s reported 33.

Steps 10 and 11 on the client-list path, `I-new-cli_new-srv-tcp_clientlist_drop_tcp-drop/client.log`
(ping lines omitted):

```
[   1.627] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=UDP
[   1.629] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=TCP_ONCE
[   1.629] EVENT CLM_TCP_OFFERED received from 127.0.0.1:35000 for msgid=1013 token=0
[   4.252] ACTION CreateCLServerListReqConnClientsListMes (listed=-1, reported=33)   <- no SEND follows (step 11)
[   4.757] QT: - Jamulus-TCP: timeout connecting to "127.0.0.1:35000"             <- 3.13 s after TCP_ONCE (step 10)
[   6.759] ACTION CreateCLServerListReqConnClientsListMes (listed=-1, reported=33)
[   6.759] SEND msgid=1014 bytes=9 to 127.0.0.1:35000 mode=UDP                    <- back on UDP (step 10)
```

On the client-list path the re-request is driven by the 2.5 s ping timer, not the 2 s server-list timer. While
TCP is pending, `CreateCLServerListReqConnClientsListMes` ignores it the same way.

The refused row shows `SEND ... mode=UDP`, then `mode=TCP_ONCE`, then the refusal within 2 ms, in every cycle.
The pending entry is removed on refusal, so the next ping sends UDP again (step 10).

Commands (one server setup at a time, so the CPU load stays at 34 clients):

```bash
./start-clsrv.sh old 34000 "" 34
./run-cell.sh G-old-cli_old-srv_clientlist_nodrop old clientlist 127.0.0.1:34000 42000 12 no
./run-cell.sh G-old-cli_old-srv_clientlist_drop old clientlist 127.0.0.1:34000 42000 12 yes
./run-cell.sh G-new-cli_old-srv_clientlist_nodrop new clientlist 127.0.0.1:34000 42100 12 no
./run-cell.sh G-new-cli_old-srv_clientlist_drop new clientlist 127.0.0.1:34000 42100 12 yes
./stop-clsrv.sh
```
```bash
./start-clsrv.sh new 35000 --enabletcp 34
./run-cell.sh H-old-cli_new-srv-tcp_clientlist_nodrop old clientlist 127.0.0.1:35000 42200 12 no
./run-cell.sh H-old-cli_new-srv-tcp_clientlist_drop old clientlist 127.0.0.1:35000 42200 12 yes
./run-cell.sh H-new-cli_new-srv-tcp_clientlist_nodrop new clientlist 127.0.0.1:35000 42300 12 no
DURING="sleep 4.5; date -u +%T.%N; echo kill client34 pid <PID>; kill <PID>" ./run-cell.sh H-new-cli_new-srv-tcp_clientlist_drop_disconnect new clientlist 127.0.0.1:35000 42400 12 yes
./run-cell.sh I-new-cli_new-srv-tcp_clientlist_drop_tcp-reject new clientlist 127.0.0.1:35000 42500 12 yes reject
./run-cell.sh I-new-cli_new-srv-tcp_clientlist_drop_tcp-drop new clientlist 127.0.0.1:35000 42600 12 yes drop
STRACE=1 ./run-cell.sh I2-strace-new-cli_new-srv-tcp_clientlist_drop_tcp-reject new clientlist 127.0.0.1:35000 42700 7 yes reject
./stop-clsrv.sh
```
```bash
./start-clsrv.sh new 36000 "" 34
./run-cell.sh J-new-cli_new-srv-notcp_clientlist_nodrop new clientlist 127.0.0.1:36000 42800 12 no
./run-cell.sh J-new-cli_new-srv-notcp_clientlist_drop new clientlist 127.0.0.1:36000 42800 12 yes
./stop-clsrv.sh
```

`<PID>` was 7817, the pid of client 34 from `logs/clsrv/clsrv-new-35000/clients.pid`.

## docs/TCP.md "Enhancement for TCP support" steps 2-11 vs the runs

| Step | Doc says | Run shows | Match |
|---|---|---|---|
| 2 | directory sends TCP_OFFERED(CLM_SERVER_LIST) after RED and full list, on UDP | tcpdump order 1285, 2708, 15 bytes in every cell | yes |
| 2a | old client ignores it | A1, A2: OLD client logs nothing, keeps UDP | yes |
| 2b, 2c | new client receives it after the UDP list and does not open TCP | smoke-new-tcp-nodrop: 0 SYN | yes |
| 2d | without the list, client opens TCP and re-requests | smoke2: SYN at 1.618 s, `SEND ... mode=TCP_ONCE` | yes |
| 2a (session 2) | old client ignores it; OLD directory sends none | D, E: 0 datagrams of 15 B from OLD directory and from NEW directory without TCP | yes |
| 3, 3b | directory answers the TCP request with CLM_SERVER_LIST only | smoke2, R1: list arrived via TCP; no RED list over TCP in client log | yes |
| 3a | no CLM_SEND_EMPTY_MESSAGE after the TCP request | F-step3a drop: 50 after the UDP request, 0 after the TCP SYN; nodrop control: 50 for one UDP request | yes |
| 4 | client closes TCP, stops the 2 s timer | smoke2, R1: no later `TIMER` line; code closes socket at `tcpconnection.cpp:181` | yes |
| 4 (re-fetch) | after CFM_TCP_RESULT a re-fetch goes straight to TCP | R3: second request `mode=TCP_ONCE` 1 ms after the call, list via TCP; R4 control after a UDP result: second request `mode=UDP` | yes |
| 5, 6 | pings and client-list requests stay on UDP | C0, G, H, J: `mode=UDP` for 1002 and 1014 | yes |
| 7 | server sends TCP_OFFERED(CLM_CONN_CLIENTS_LIST) after the UDP list | C0: 165 B list then 15 B offer; H: 1777 B list then 15 B offer, `msgid=1013` | yes |
| 7a | old client ignores it | H-old nodrop and drop: offers arrive, OLD client logs nothing, 0 SYN | yes |
| 7c | list already received, no TCP | C0, H-new nodrop: 0 SYN | yes |
| 7d | list lost, client opens TCP and re-requests | H-new drop: `mode=TCP_ONCE` 1 ms after the UDP request, SYN on port 35000 | yes |
| 8 | server answers the TCP request over TCP | H-new drop: `TRANSPORT CLM_CONN_CLIENTS_LIST via TCP entries=34` 2 ms later | yes |
| 9 | after CFM_TCP_RESULT the next count change requests via TCP at once | H-new drop: count 34 to 33 at 6.442 s, `SEND msgid=1014 ... mode=TCP_ONCE` at 6.442 s, no UDP 1014 after 1.484 s, 33 entries via TCP at 6.445 s | yes |
| 10 | TCP fails or times out, revert to UDP | B1, I-reject: refused, next request sends UDP; B2: timeout after 3.15 s, next tick sends UDP; I-drop: timeout after 3.13 s, next ping (6.759 s) sends UDP | yes |
| 11 | re-request ignored while TCP pending | B2: 2 s tick at 3.718 s produced no request; I-drop: ping-driven request at 4.252 s produced no SEND | yes |

No mismatch between docs/TCP.md and the runs was found in any cell. Observation, not a mismatch: after
a TCP failure the reversion to UDP happens at the next timer tick, not immediately. With a dropped SYN the cycle
is UDP request, offer, 3 s TCP timeout, UDP request at the following tick. That gives a 4 s period in B2 and 5 s in I-drop.

## Outbound registration check

Session 1:

`tcpdump -i eth0 -n 'not (tcp port 443) and not (tcp port 80) and not arp'` for 25 s while the 51-process lab
was running captured 2 packets: one DNS query for `http-intake.logs.us5.datadoghq.com` from the container's
own tooling and its answer. No packet to or from UDP/TCP 22124 and no Jamulus traffic left the host
(`logs/eth0-outbound.txt`). Server logs mention only `127.0.0.1:30000` as directory; the strings
`anygenre`, `jamulus.app`, `jamulus.io` do not occur in any server log.

Session 2, same filter, two captures:

| Capture | Window (UTC) | Covers | Packets |
|---|---|---|---|
| `logs/eth0-outbound2.txt` | 22:18:11 to 22:20:31 | start of lab 30000 at 22:18:12, finished 22:18:20, plus 131 s; cells R1 to R4 | 2: DNS query for `api.anthropic.com` to 8.8.8.8 and its answer |
| `logs/eth0-outbound3.txt` | 22:25:03 to 22:30:58 | all three client-list server setups and cells G to J | 8: DNS queries for `http-intake.logs.us5.datadoghq.com` (3) and `api.anthropic.com` (1) to 8.8.8.8, and their answers |

No packet to or from port 22124 and no Jamulus traffic left the host in either capture. Labs 31000, 32000 and
33000 ran between the two captures (directory logs created 22:22:06, 22:22:59 and 22:23:35, last log write 22:24:41) with no eth0 capture. Their 150 server logs all read
`register with directory at address: 127.0.0.1:31000`, `127.0.0.1:32000` or `198.51.100.10:33000`. The three
client-list servers were started without `--directoryaddress` and their logs have no `register with` line.
`grep -rilE 'anygenre|jamulus\.app|jamulus\.io|22124'` over every session 2 server and client log matches 0
files. Server and headless client logs contain no error or warning lines.

## NOT covered

Not covered by any run: IPv6; real IP fragment loss on a network path (simulated by the length rule on lo); the
GUI Connect dialog itself (the driver calls the same CClient methods and timers, but the dialog was not
executed); step 3a with a client on a non-private address (the client was 127.0.0.1, only the servers were
non-private); OLD headless clients connected to a NEW server and vice versa (each client-list server had clients
of its own tree); the `CLM_TCP_OFFERED` sent before `CLIENT_ID` on connect and all connected-mode TCP behaviour
beyond counting the established sessions; an eth0 capture during labs 31000 to 33000.
