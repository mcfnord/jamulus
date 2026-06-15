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

**Pattern list coordination**: Server builds fetch their pattern list from
`https://jamulus.live/chat-patterns.txt` at startup (refreshed hourly). Client builds
use the hardcoded `kPatterns[]` array at `chatreporter.cpp:40`. JamFan22 also validates
incoming `/chat-url-client` reports against `chat-patterns.txt` as a defense-in-depth
filter. **The two lists must be identical.** A pattern missing from `chat-patterns.txt`
is silently dropped by JamFan22; a pattern missing from `kPatterns[]` is never reported
by client builds. When adding a domain to either place, add it to both.

Also handles `/stream` chat commands: intercepts `/stream` text, POSTs
`{"command":"stream","port":N,"weekly":false}` to `https://jamulus.live/chat-command-server`,
and broadcasts the response back to all connected clients.

`reportClientInfo(addr, name, countryId, instrument)` fires on each `ChanInfoHasChanged`
event. Computes GUID as MD5(name + phpCountryName(countryId) + phpInstrumentName(instrument))
matching the ping-join Python format, then GETs `/player-identified/{ip}?serverport=...&guid=...&channelId=...&nation=...`.

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

- **Deploy PCM-capable binary to all fleet servers**: Once PCM audio is confirmed working end-to-end (client connects to Freiheit with AQ_RAW and negotiates uncompressed audio), run `./deploy.sh all` to push the new `jamfan` binary to all x86-64 fleet servers. Freiheit (aarch64) is already updated. The Duet servers (7 headless ping-harvester servers on 24.199.107.192) are not fleet servers — skip them. Do not deploy until end-to-end test passes.

- **Windows client**: wire chat-reporter URL detection into the GUI client build; make the blue Jamulus logo open `https://jamulus.live`. Build via GitHub Actions: `git push origin jamfan:autobuild-jamfan`, download artifact, delete remote branch.

- **Client URL abuse monitoring in JamFan22** (C# on `jamulus.live`, must be done before wide distribution): reuse ip-allowed block logic, apply `chat-patterns.txt` server-side (defense-in-depth), rate-limit by IP, fail closed, log source IP. Details in `jamulus/TODO.md`. Hand off to `claude` on `jamulus.live`.

- **Windows client announcement + binary palette rollout**: announce to fleet users via welcome message once PCM + URL filtering are confirmed; publish GitHub Release for stable download URL; dismissable per-GUID banner; server binary; changelog at `jamulus.live/jamfan`. Details in `jamulus/TODO.md`.

- **Propose `getClients` identification gate upstream**: After end-to-end testing, open a PR against `jamulussoftware/jamulus` for the two-line fix in `channel.h` (`IsIdentified()` getter) and `server.cpp` (`GetConCliParam` filter changed to `IsConnected() && IsIdentified()`). This is a generic correctness fix — `getClients` returning default-empty fields for channels in the pre-identification window is a bug for any RPC consumer, not just JamFan22. Unlike the JamFan features, this has no custom logic and is a clean upstream candidate.
  - **Status**: PR submitted (draft): https://github.com/jamulussoftware/jamulus/pull/3716. Committed to `jamfan` as `bf1e951b`. **Note**: this fix addresses the timing race (unidentified client briefly visible with empty fields) but does NOT fix JamFan22's welcome self-exclusion bug — that bug is caused by JamFan22 reading wrong field names from the response (see `getClients` field name mismatch TODO below), not by timing.

- **GUID computation standing trap**: never use `CInstPictures::GetName()` for GUID computation — diverges from `phpInstrumentName` at indices 0 (`"None"` vs `"-"`), 1 (`"Drum Set"` vs `"Drums"`), 26, and 27. JamFan22's `GetClientsAsync` uses `_instrumentNames[]` (matching `phpInstrumentName`); GUID parity confirmed 2026-06-11 (92.3% match across 274 fleet join events).

- **Public RPC port — unauthenticated status API** (future): A second TCP listener on port 22224 (same number as the Jamulus UDP game port — no conflict since protocols differ). No `--jsonrpcport` or secret required; enabled by passing `--publicrpcport` flag (no argument). Operators who don't want it simply omit the flag. Because the port is well-known (always 22224/tcp), any client who knows a server's address can query it without asking the operator for a port number.

  **Methods to expose on the public port** (all read-only, no privacy concern):
  - `jamulus/getVersion` — server version
  - `jamulus/getMode` — always `"server"` on a server binary
  - `jamulusserver/getServerProfile` — name, city, country, welcome message, directory status (already public via directory)
  - `jamulusserver/getRecorderStatus` — recording on/off
  - `jamulusserver/getClients` — **omit the `address` field** (IP:port is private; name/instrument/country are already visible to session participants)

  **Implementation**: `CRpcServer` gets a `HandlePublicMethod()` variant that adds to a separate `mapPublicMethodHandlers`. `ProcessMessage` checks that map before the auth gate. The public-port server is a second `CRpcServer` instance constructed with an empty secret; all methods registered on it via `HandlePublicMethod` are automatically unauthenticated. Fleet service files get `--publicrpcport` added; firewall rules open 22224/tcp to the world on each server.

## Branch strategy
`main` tracks upstream exactly — no custom commits. `jamfan` is rebased onto `main`
(history rewrite is acceptable). Pull upstream into `main`, then `git rebase main jamfan`.

## ARM64 deploy pitfalls

**Never rsync the local `Jamulus` binary to ARM64 hosts.** The local build produces an x86-64 binary; rsyncing it to Turin/Freiheit/Rising without `--exclude='Jamulus'` overwrites the correct aarch64 binary with a broken one (silent failure — rsync succeeds, `sudo mv` succeeds, service crashes with "Exec format error"). Always add `--exclude='Jamulus'` when rsyncing sources to ARM64 build hosts.

**Turin (Oracle Linux 9) additionally requires `qt5-linguist`** for `lrelease`. Without it, sequential `make` fails on the translation target before the linker runs and no binary is produced. Install once with `sudo dnf install -y qt5-linguist`; already done as of 2026-06-08.
