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

The binary is `./Jamulus`. Run as a server with `-s -n --nogui`.

Enable custom feature logging:
```bash
QT_LOGGING_RULES="jamulus.chatreporter=true;jamulus.centraldefense=true" ./Jamulus -s -n --nogui
```

## Custom Features (all live in `jamfan`)

### `central-defense`
`src/centraldefense.cpp`: rejects server connections based on a per-IP lookup.
Calls `GET https://jamulus.live/ip-allowed/{ip}` — returns `"true"` (blocked) or
`"false"` (allowed). Fails open on any network error. Timeout: 2 seconds.

### `chat-reporter`
`src/chatreporter.cpp`: scans chat messages for URLs matching patterns fetched from
`https://jamulus.live/chat-patterns.txt` (refreshed hourly). On match, POSTs
`{"url": "..."}` to `https://jamulus.live/chat-url`. Only the URL is sent — full
message text is discarded. Fails silently if either endpoint is unreachable.
Hooks into server (raw message text) and client (`ChatTextReceived` signal).

### `earlier-join-notification`
Fires a connection notification at the earliest socket step, before the full handshake
completes. Modifies `socket.cpp` and `serverlogging`.

### `make_welcome`
Debian packaging: `postinst` creates `/etc/jamulus/welcome.html` on install. The systemd
service already passes `-w /etc/jamulus/welcome.html` to the binary.

## Planned Custom Features (not yet implemented)

### Recording banner API
The server already exposes an RPC/API. Extend it with a method to toggle the red
RECORDING banner on all connected clients — the same banner shown when the server itself
is recording. This allows an external recording tool to signal participants that recording
is in progress. No new protocol needed; extend the existing server API surface.

## Branch strategy
`main` tracks upstream exactly — no custom commits. `jamfan` is rebased onto `main`
(history rewrite is acceptable). Pull upstream into `main`, then `git rebase main jamfan`.
