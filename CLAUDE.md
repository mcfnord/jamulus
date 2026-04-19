# Jamulus Fork — mcfnord/jamulus

This is a fork of [jamulussoftware/jamulus](https://github.com/jamulussoftware/jamulus).
The `main` branch tracks upstream. Custom work lives in the `jamfan` branch.
Changes here are not expected to be accepted upstream.

## Building

Qt 5.15 is required (available in standard Ubuntu/Debian repos).

```bash
sudo apt-get install -y build-essential qtbase5-dev qt5-qmake qtmultimedia5-dev \
    qttools5-dev-tools libjack-jackd2-dev
qmake "CONFIG+=headless" Jamulus.pro
make -j$(nproc)
```

The binary is `./Jamulus`. Run as a server with `--server --nogui`.

Enable custom feature logging:
```bash
QT_LOGGING_RULES="jamulus.chatreporter=true;jamulus.centraldefense=true" ./Jamulus -s -n --nogui
```

## Custom Features (all live in `jamfan`)

### `central-defense`
`src/centraldefense.cpp`: rejects server connections based on a per-IP lookup.
Calls `GET https://jamulus.live/ip-allowed/{ip}` — returns `"true"` (allowed) or
`"false"` (blocked). Fails open on any network error. Timeout: 2 seconds.

The check happens synchronously at the UDP socket level (`socket.cpp`) via
`CServer::CentralDefenseAllows()` → `CentralDefense::shouldAllow()`, before
`PutAudioData` is called. Blocked IPs never get a channel allocated.

Cache TTLs: blocked = 5 minutes, allowed = 1 hour. On timeout/error, IP is
cached as allowed (fail-open) for 1 hour to avoid hammering a down backend.

Local allowlist: `/etc/jamulus/ip-allowlist.txt` — one IP or CIDR per line,
checked before any network lookup. `#` = comment, `!` prefix is stripped and
the IP is still allowed.

### `chat-reporter`
`src/chatreporter.cpp`: scans chat messages for URLs matching patterns fetched from
`https://jamulus.live/chat-patterns.txt` (refreshed hourly). On match, POSTs
`{"url": "...", "port": N}` to `https://jamulus.live/chat-url-server` (server builds)
or `https://jamulus.live/chat-url-client` (client builds). For server builds, the
server IP is inferred from the TCP connection and combined with `port` to form
`<ip>:<port>`. For client builds, `port=0` is sent; jamulus.live derives the server
via client-IP → guid → server lookup (TODO in JamFan22). Full message text is
discarded. Fails silently if either endpoint is unreachable.
Hooks into server (raw message text) and client (`ChatTextReceived` signal).

### `earlier-join-notification`
Fires a connection notification at the earliest socket step, before the full handshake
completes. Modifies `socket.cpp` and `serverlogging`.

### `make_welcome`
Debian packaging: `postinst` creates `/etc/jamulus/welcome.html` on install. The systemd
service already passes `-w /etc/jamulus/welcome.html` to the binary.

### `recording-banner-api`
Extends `src/serverrpc.cpp` with `jamulusserver/setRecordingBanner`. Accepts
`{"active": true|false}`. When `true`, overrides the recorder state sent to all
connected clients with `RS_RECORDING` (the red RECORDING banner), without starting
the actual server recorder. Clears on `false`. Implemented via
`CServer::m_bExternalRecordingBanner` flag checked in
`CreateAndSendRecorderStateForAllConChannels`.

## Production server — "Hot Texas"

Host: `50.116.25.151`, user: `root`, SSH key: `~/.ssh/id_ed25519`
Service: `jamulus-headless.service`

Deployment steps:
1. Build: `qmake "CONFIG+=headless" Jamulus.pro && make -j$(nproc)`
2. Stop service: `ssh root@50.116.25.151 'systemctl stop jamulus-headless'`
3. Copy binary: `scp Jamulus root@50.116.25.151:/usr/bin/jamulus-jamfan`
4. Start service: `ssh root@50.116.25.151 'systemctl start jamulus-headless'`

**Important**: the service must be stopped before `scp` — the binary cannot be overwritten while running.

## JamFan22 — backend at jamulus.live

Host: `root@jamulus.live`, SSH key: `~/.ssh/id_ed25519`
Service: `jamfan22.service` (ASP.NET Core 9, port 443)
Log: `/root/JamFan22/JamFan22/output.log`
Source: `/root/JamFan22/`

Endpoints consumed by this Jamulus fork:
- `GET /ip-allowed/{ip}` — central-defense IP check
- `GET /chat-patterns.txt` — URL patterns for chat-reporter
- `POST /chat-url-server` — receives `{"url": "...", "port": N}`; derives server as `<remoteIP>:<port>`
- `POST /chat-url-client` — receives `{"url": "..."}` from client builds; derives server via client-IP → guid → server (TODO)

## TODO

- **Windows client**: wire chat-reporter URL detection into the GUI client build; make the blue Jamulus logo open `https://jamulus.live`. Build via GitHub Actions: `git push origin jamfan:autobuild-jamfan`, download artifact, delete remote branch.

## Branch strategy
`main` tracks upstream exactly — no custom commits. `jamfan` is rebased onto `main`
(history rewrite is acceptable). Pull upstream into `main`, then `git rebase main jamfan`.
