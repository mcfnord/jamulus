# Jamulus Fork TODOs

## Windows client

**Windows client announcement**: Once Windows client artifact is confirmed working (PCM audio + client URL filtering in place), announce to existing fleet server users via JamFan22 dynamic welcome for a limited window (~2 weeks). Decide mechanism (welcome addendum, jamulus.live page banner, or gojam chat) before implementing.

**Binary palette rollout** (after announcement is confirmed):
- Client binary: GH Actions autobuild path (`git push origin jamfan:autobuild-jamfan`) already works. Publish to a GitHub Release on `mcfnord/jamulus` (tag e.g. `jamfan-client-latest`) for a stable, non-expiring download URL. Update `jamulus.live/jamfan` with a prominent download button and one-line delta summary (PCM audio, clickable logo → jamulus.live, URL sharing).
- Dismissable banner: fleet users discover the client via JamFan22 dynamic welcome. One sentence + download link, shown only once per GUID. Store dismissal state per GUID in JamFan22. Gates on announcement TODO above.
- Server binary: Publish a Linux x86-64 server binary alongside the client release. Less urgent than client.
- Changelog page: `jamulus.live/jamfan` should list the delta from upstream (PCM audio, logo link, URL sharing, central-defense). One paragraph, no jargon.
- First tiny step: push `jamfan` to `autobuild-jamfan`, download artifact, smoke-test it, then create a GitHub Release at `mcfnord/jamulus` with that artifact and a `jamulus.live/download` redirect.

## Connectionless CLM_REQ_CHANNEL_LEVEL_LIST (next up)

`CLM_CHANNEL_LEVEL_LIST` (ID 1015) already exists as a connectionless UDP message — the server sends it periodically to connected clients that have opted in via a session-level message. This work makes it requestable by anyone without a full Jamulus session.

**Plan**:

1. **C++ (`src/protocol.h`, `src/protocol.cpp`, `src/server.cpp`)** — add `PROTMESSID_CLM_REQ_CHANNEL_LEVEL_LIST` (ID 1019) as a new connectionless message ID. When the server receives it on the game port, it sends the current `CLM_CHANNEL_LEVEL_LIST` back to the requester's address. Implementation follows the exact pattern of `CLM_REQ_CONN_CLIENTS_LIST` → `CLM_CONN_CLIENTS_LIST`: new ID in `protocol.h`, case in the switch, `EvaluateCLReqChannelLevelListMes` emits a signal, server slot calls `CreateCLChannelLevelListMes`. No new response format needed — reuses the existing message.

2. **JamFan22 (C# on `jamulus.live`)** — poll all active servers (fleet + directory) by sending `CLM_REQ_CHANNEL_LEVEL_LIST` as a raw UDP datagram, parse the `CLM_CHANNEL_LEVEL_LIST` response (4-bit levels packed two per byte), determine quiet if all levels are zero. Store per-server quiet status for use by welcome message queuing and the lounge listener UI. Hand off to `claude` on `jamulus.live`.

3. **Upstream PR** — the C++ change is a clean generic improvement with no jamfan-specific logic. Submit against `jamulussoftware/jamulus` after the fleet is running it.

**Servers that don't support the new ID silently ignore it** (no `default:` case in the protocol switch) — safe to send to any public server.

**Future / deferred**: a `secondsSinceAudio` uint32 counter (reset to 0 on activity, incremented each silent second) would give duration history without a session. Not needed once JamFan22 is polling levels directly. Revisit only if duration history proves useful.

## Client URL abuse monitoring in JamFan22 (C# on jamulus.live)

The `/chat-url-client` endpoint will receive URLs from an unknown number of Windows client installs. Treat like a public inbound API:
- **Reuse ip-allowed block logic**: reject submissions from IPs already flagged as blocked
- **Defense-in-depth pattern filtering**: apply `chat-patterns.txt` server-side before logging any URL
- **Rate limiting by IP**: cap URL submissions per IP per hour; use short-lived cache analogous to ip-allowed cache TTLs
- **Fail closed**: unlike ip-allowed, `/chat-url-client` can silently drop submissions when overloaded — missing a URL is harmless
- **Per-IP logging**: store source IP alongside url/timestamp for every accepted submission

Must be in place before the Windows client is widely distributed. Hand off to `claude` on `jamulus.live`.

## CLM_RAWAUDIO_SUPPORTED — connectionless raw audio capability advertisement

**Status**: Implemented and building clean on `jamfan` branch. Not yet tested end-to-end. Upstream PR candidate once validated.

**What was built**: `CLM_RAWAUDIO_SUPPORTED` (ID 1036) and `CLM_REQ_RAWAUDIO_SUPPORTED` (ID 1037) — a connectionless request/reply pair mirroring the `CLM_VERSION_AND_OS` / `CLM_REQ_VERSION_AND_OS` pattern. A poller sends 1037; the server replies with 1036 (empty payload) if and only if `!bDisableRaw`. No reply = raw audio not supported. Fully backward compatible — servers that don't know the message silently ignore it.

**Milestones before upstream PR**:

1. **End-to-end test (local)**: Start a local `jamfan` server, send a raw UDP `CLM_REQ_RAWAUDIO_SUPPORTED` datagram to its game port, confirm a `CLM_RAWAUDIO_SUPPORTED` reply arrives. Repeat with `--disable-raw` flag and confirm no reply. A minimal Python script using `struct.pack` to build the connectionless message frame (tag `0x0000`, message ID `1037`, sequence, data length `0`, checksum) is all that's needed.

2. **Fleet validation**: Deploy to one fleet server (e.g. Freiheit after native ARM64 build), probe with the Python script from `jamulus.live`, confirm correct behavior.

3. **Consumer integration**: Update `servers.php` (or equivalent poller) to include `CLM_REQ_RAWAUDIO_SUPPORTED` in its per-server probe alongside `CLM_REQ_VERSION_AND_OS`. Expose `rawaudio: true` in JSON output. Verify the field appears for fleet servers and is absent for non-fleet servers.

4. **Upstream PR**: The change is generic, has no jamfan-specific logic, and follows an established pattern. Submit against `jamulussoftware/jamulus` with the test results as evidence.
