# Jamulus Fork TODOs

## fleet-stress.sh — untested, run in next quiet window

`/root/jamulus/fleet-stress.sh` — validates the CentralDefense TTL fix (deployed 2026-06-27, TTL 20s→600s). Needs a quiet window (04:30 UTC):
- `TEST=regression ./fleet-stress.sh Freiheit --noise` — confirm zero `cache-miss` lines at t=20s
- `TEST=boundary ./fleet-stress.sh Freiheit --noise` — confirm no degradation at 600s ±30s TTL window
- Still TODO: add lounge IP (`147.182.199.22`) to `/etc/jamulus/ip-allowlist.txt` on every fleet server.

## Upstream PR: getClients identification gate

Draft PR submitted: https://github.com/jamulussoftware/jamulus/pull/3716 (committed to `jamfan` as `bf1e951b`). Two-line fix: `IsIdentified()` getter in `channel.h`, `IsConnected() && IsIdentified()` filter in `server.cpp` — generic correctness fix, clean upstream candidate. Note: this fixes the timing race only, NOT JamFan22's old welcome self-exclusion bug (that was wrong field names — `instrumentCode`/`countryName`; documented in `JamFan22/JamFan22/CLAUDE.md`). Follow-up: check whether `[WARN-WELCOME-SELF-IN-ROOM]` still fires in production output.log; if gone, close that thread.

## Windows client

**Wire-up remaining**: chat-reporter URL detection in the GUI client build; blue Jamulus logo opens `https://jamulus.live`.

**Announcement** (after artifact confirmed working — PCM audio + client URL filtering): announce to fleet users via JamFan22 dynamic welcome for ~2 weeks. Decide mechanism (welcome addendum, site banner, or gojam chat) first.

**Binary palette rollout** (after announcement):
- Publish a GitHub Release on `mcfnord/jamulus` (e.g. tag `jamfan-client-latest`) for a stable download URL; button + one-line delta on `jamulus.live/jamfan`.
- Dismissable per-GUID welcome banner (one sentence + link, shown once; dismissal stored in JamFan22).
- Linux x86-64 server binary alongside (less urgent).
- Changelog paragraph at `jamulus.live/jamfan` (PCM audio, logo link, URL sharing, central-defense — no jargon).
- First tiny step: push `jamfan:autobuild-jamfan`, smoke-test artifact, create the Release, add a `jamulus.live/download` redirect.

**Video link false-positive**: the client treats video URLs in a server's static welcome/MOTD as live streams. Fix: only count URLs received outside the initial message burst on connect (`chatreporter.cpp`).

## Client URL abuse monitoring in JamFan22 (C# on jamulus.live — prerequisite for wide client distribution)

`/chat-url-client` will receive URLs from unknown installs. Treat as a public inbound API:
- Reuse ip-allowed block logic (reject blocked IPs)
- Apply `chat-patterns.txt` server-side (defense-in-depth)
- Rate-limit per IP per hour (short-lived cache, like ip-allowed TTLs)
- Fail closed (dropping a URL is harmless)
- Log source IP per accepted submission

Hand off to `claude` on `jamulus.live`.

## Client-side song detection (not yet implemented)

Server builds detect `Title – Key` chat messages → POST `/chat-song-server`. Client builds need the equivalent for non-fleet servers:
- **Extraction**: `reportIfMatch` receives `<font …>(hh:mm:ss AP) <b>Name</b></font> Message`; split on `</font> `, parse `<b>…</b>` for sender.
- **New endpoint**: POST `/chat-song-client`; JamFan22 derives the server via client IP → guid → server, same as `/chat-url-client`.
- **JamFan22 side**: same IP-gating and rate-limiting as `/chat-url-client` before wide distribution.

## Public RPC port — unauthenticated status API (future)

Second TCP listener on 22224 (no clash with the UDP game port), enabled by a new `--publicrpcport` flag, no secret. Expose read-only: `jamulus/getVersion`, `jamulus/getMode`, `jamulusserver/getServerProfile`, `getRecorderStatus`, `getClients` **minus the `address` field**. Implementation: second `CRpcServer` with empty secret + `HandlePublicMethod()`/`mapPublicMethodHandlers` checked before the auth gate. Fleet service files gain the flag; firewall opens 22224/tcp to the world.
