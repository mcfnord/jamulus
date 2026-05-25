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

Cache TTLs: blocked = 5 minutes, allowed = 2 minutes. On timeout/error, IP is
cached as allowed (fail-open) for 2 minutes to avoid hammering a down backend.

Local allowlist: `/etc/jamulus/ip-allowlist.txt` — one IP or CIDR per line,
checked before any network lookup. `#` = comment, `!` prefix is stripped and
the IP is still allowed.

### `chat-reporter`
`src/chatreporter.cpp`: extracts all `https?://` URLs from chat messages and POSTs
each as `{"url": "...", "port": N}` to `https://jamulus.live/chat-url-server` (server builds)
or `https://jamulus.live/chat-url-client` (client builds). For server builds, the
server IP is inferred from the TCP connection and combined with `port` to form
`<ip>:<port>`. Fails silently if the endpoint is unreachable.
Hooks into server (raw message text) and client (`ChatTextReceived` signal).

Also handles `/stream` chat commands: intercepts `/stream` text, POSTs
`{"command":"stream","port":N,"weekly":false}` to `https://jamulus.live/chat-command-server`,
and broadcasts the response back to all connected clients.

`reportClientInfo(addr, name, countryId, instrument)` fires on each `ChanInfoHasChanged`
event. Computes GUID as MD5(name + phpCountryName(countryId) + phpInstrumentName(instrument))
matching the ping-join Python format, then GETs `/ip-allowed/{ip}?guid=<hash>`.

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

- **Deploy PCM-capable binary to all fleet servers**: Once PCM audio is confirmed working end-to-end (client connects to Freiheit with AQ_RAW and negotiates uncompressed audio), run `./deploy.sh all` to push the new `jamfan` binary to all x86-64 fleet servers. Freiheit (aarch64) is already updated. Duet is not a fleet server — skip it. Do not deploy until end-to-end test passes.

- **Windows client**: wire chat-reporter URL detection into the GUI client build; make the blue Jamulus logo open `https://jamulus.live`. Build via GitHub Actions: `git push origin jamfan:autobuild-jamfan`, download artifact, delete remote branch.

- **Update `chat-patterns.txt` on jamulus.live for JamFan22 defense-in-depth** (`/root/JamFan22/JamFan22/wwwroot/chat-patterns.txt`): two changes needed: (1) broaden the Ultimate Guitar pattern from `ultimate-guitar\.com/tab/` to `ultimate-guitar\.com/[^\s]*` to cover `/user/tab/view` and `/user/playlist/shared` paths; (2) add `https://vocal-voyage\.de/[^\s]*`. These patterns are already compiled into the client binary — this update is for the JamFan22 server-side defense-in-depth filter on `/chat-url-client`. Hand off to `claude` on `jamulus.live`.

- **Client-side URL pattern filtering** (`src/chatreporter.cpp`): DONE. Client builds load 13 patterns compiled into the binary at startup (`#else SERVER_BUNDLE` branch of `start()`); `reportIfMatch()` skips any URL that matches none of them. Server builds continue sending all URLs unfiltered. Pattern list is auditable in source — changing it requires a new binary.

- **Client URL abuse monitoring in JamFan22** (C# on `jamulus.live`): The `/chat-url-client` endpoint will receive URLs from an unknown number of Windows client installs. Treat it like a public inbound API with the same trust model as `/ip-allowed`:
  - **Reuse ip-allowed block logic**: if an IP is already flagged as blocked by the ip-allowed subsystem, reject its `/chat-url-client` submissions too — same "known bad" signal applies. Avoids building parallel block infrastructure.
  - **Defense-in-depth pattern filtering**: apply `chat-patterns.txt` server-side before logging any URL, even though the C++ client also filters. Two independent checks; a malicious or unpatched client can't bypass both.
  - **Rate limiting by IP**: cap URL submissions per IP per hour; use a short-lived cache for rate-limit decisions (analogous to ip-allowed cache TTLs).
  - **Fail closed**: unlike ip-allowed (which fails open because a missed connection matters), `/chat-url-client` can silently drop submissions when the backend is overloaded — missing a URL is harmless.
  - **Per-IP logging**: store source IP alongside url/timestamp for every accepted submission; surface abuse metrics in the admin log.
  - Must be in place before the Windows client is widely distributed. Hand off to `claude` on `jamulus.live`.

- **Windows client announcement**: Once the Windows client artifact is confirmed working (PCM audio + client URL filtering in place), announce availability to existing fleet server users via the JamFan22 dynamic welcome message for a limited window (e.g. 2 weeks). Message should link to the download and mention PCM audio quality. Decide announcement mechanism (welcome message addendum, jamulus.live page banner, or gojam chat) before implementing.

- **Send player name in /ip-allowed requests** (`src/chatreporter.cpp`, one line): `reportClientInfo` already has `name` in scope but doesn't send it. Add `if (!name.isEmpty()) query.addQueryItem(QStringLiteral("name"), name);` after the `guid` line. **Note**: JamFan22 already retrieves player names via `getClients` RPC at welcome time, so this C++ change is only necessary to capture names for **blocked players** (who never receive a welcome). For welcomed players, a C#-only change on JamFan22 can write names to `fleet-guid-ip.csv` without touching C++.

- **Always send GUID in /ip-allowed requests** (`src/chatreporter.cpp`, `central-defense`): When a fleet server calls `GET /ip-allowed/{client_ip}?guid={client_guid}`, blocked connections currently omit the `guid` parameter (JamFan22 logs show `guid=-`). The GUID is available from the Jamulus protocol handshake before the check is made — it should be included regardless of outcome. Fix is in the code path that constructs the `/ip-allowed` URL. Blocked connections are the most important ones to track in `fleet-guid-ip.csv`, so this gap matters.

## Branch strategy
`main` tracks upstream exactly — no custom commits. `jamfan` is rebased onto `main`
(history rewrite is acceptable). Pull upstream into `main`, then `git rebase main jamfan`.
