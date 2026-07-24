# Jamulus Fork TODOs

## Upstream PR #3739 (autovectorization): benchmark posted 2026-07-19

Posted benchmark comment supporting ann0see's one-line `-ftree-vectorize` PR: https://github.com/jamulussoftware/jamulus/pull/3739#issuecomment-5015335432 (g++ 13.3: `-O2` vectorizes 0 mix loops, PR flag vectorizes all 8 = same set as `-O3`, 2.2–3.8× faster, bit-exact, MSVC inert). Watch for maintainer/dingodoppelt response; benchmark source offered on request (durable copy: `/root/mixbench-3739.cpp`). Once merged upstream: rebase `jamfan` picks up the flag for free on the next fleet build — no action needed. Prune this entry after the PR resolves.

## Upstream PR #3807: tooltip toggle (rebase of stalled #3446, opened 2026-07-19)

Revived softins' approved-but-conflicting #3446 as draft #3807 (branch `tooltips-enable-disable-rebased`; softins' 2 commits authorship-preserved + 1 adaptation commit folding the ToolTip check into the `CClientSettingsDlg::eventFilter` main gained from the MIDI work). Built clean Qt 5.15, clang-format-14 clean, headless SIGTERM smoke passed. Cross-linked on #3446 with an offer to close if softins prefers reviving his own branch.

Related: pljones' #3252 (connect-dialog tooltips) is blocked on this toggle, but #3446's filters don't cover `CConnectDlg` — the toggle wouldn't govern exactly the tooltips softins objected to. Built+tested the fix (~25 lines) on branch `connectdlg-tooltips-with-filter` (fork commit `6a4617e1`, includes a merge of #3252) and offered it on the #3252 thread for pljones to cherry-pick.

Status 2026-07-19 ~05:00 UTC: full build matrix green, marked ready for review; no response yet from softins/maintainers. Next: (1) watch for softins' response and review comments; (2) if pljones doesn't pick up `6a4617e1`, open it as a follow-up PR after #3807 and #3252 land; (3) ann0see's "advanced setting?" placement question from #3446 may resurface in review — softins' answer (parallel to Audio Alerts checkbox) is in the old thread.

## Upstream PRs #3805/#3806: client connection state + JSON-RPC connect (opened 2026-07-19)

Took over stalled PR #3372 (ann0see's extract of pgScorpio's #2550; stalled Nov 2024 on pljones's state-model review). Both drafts, stacked, branches `client-connection-state` / `client-connection-rpc` on the fork:

- **#3805** — #3372 ported onto current main (pgScorpio authorship preserved) + `EConnectionState` machine in CClient (`CS_DISCONNECTED/CONNECTING/CONNECTED`; CONNECTED on channel-ID assignment; `Connected`→`Connecting` signal rename per pljones). Fixes #3367, supersedes #3372, toward #3801.
- **#3806** — `jamulusclient/connect|disconnect|getConnectionState` + `connecting|connectingFailed|connectionStateChanged` notifications; JSON-RPC.md regenerated. This is the enabler for **click-to-join in the JamFan22 web app** (browser → local bridge → RPC connect).

Smoke-tested: 16/16 scripted RPC checks (connect/reconnect-while-connected/disconnect lifecycle) against local server with `--nogui` client + jackd dummy; GUI and headless builds clean; clang-format-14 clean. Test driver: scratchpad `rpc_smoke.py` (session-lived — recreate from #3806 body if needed).

**State as of pause (2026-07-19 ~01:50 UTC):** all style checks (C-like/Python/shell) green on both PRs; full autobuild matrix was still running — NOT yet verified. Both PRs are drafts.

Next steps, in order:
1. `gh pr checks 3805/3806 --repo jamulussoftware/jamulus` — confirm the full build matrix is green (no `pending`, no `fail`).
2. Mark both PRs ready for review (`gh pr ready`), tick the "waited for checks" checkbox in each body.
3. Watch for ann0see/pljones reaction to the takeover (supersedes #3372, fixes #3367); expect state-model scrutiny on #3805 — the answers live in the #3372 Nov-2024 comment thread.
4. After #3806 lands (or on jamfan first), build the click-to-join bridge for jamulus.live: browser → local bridge → `jamulusclient/connect`.

**Click-to-join bridge — architecture note (2026-07-20):** use WebSockets, not HTTP POST, for the local `127.0.0.1` listener. Safari/WebKit restricts background HTTP fetches from an `https://` page to a local port (port-scanning concern); `ws://127.0.0.1` from an HTTPS page is treated more permissively across Chrome/Edge/Safari alike, and gets two-way push for free (status back to the tab without polling). Launch flow: client binary opens the default browser via OS call (`ShellExecute`/`start` on Windows, `open` on Mac) with a one-time auth token in the URL query string; the page then opens the WebSocket to `ws://127.0.0.1:<jsonrpcport>` and authenticates with that token before calling `jamulusclient/connect`. Binding to loopback avoids the macOS incoming-connection firewall prompt. Feeds directly into #3806's RPC surface — no new C++ needed beyond exposing the RPC port over WS instead of/alongside raw TCP.

Follow-ups surfaced: (1) cherry-pick both branches onto `jamfan` so the Windows client build gets RPC connect without waiting for upstream; (2) #3660 could rebase onto #3805's accessors — offer on that thread.

## fleet-stress.sh — untested, run in next quiet window

`/root/jamulus/fleet-stress.sh` — validates the CentralDefense TTL fix (deployed 2026-06-27, TTL 20s→600s). Needs a quiet window (04:30 UTC):
- `TEST=regression ./fleet-stress.sh Freiheit --noise` — confirm zero `cache-miss` lines at t=20s
- `TEST=boundary ./fleet-stress.sh Freiheit --noise` — confirm no degradation at 600s ±30s TTL window
- Still TODO: add lounge IP (`147.182.199.22`) to `/etc/jamulus/ip-allowlist.txt` on every fleet server.

## Upstream PR: getClients identification gate

Draft PR submitted: https://github.com/jamulussoftware/jamulus/pull/3716 (committed to `jamfan` as `bf1e951b`). Two-line fix: `IsIdentified()` getter in `channel.h`, `IsConnected() && IsIdentified()` filter in `server.cpp` — generic correctness fix, clean upstream candidate. Note: this fixes the timing race only, NOT JamFan22's old welcome self-exclusion bug (that was wrong field names — `instrumentCode`/`countryName`; documented in `JamFan22/JamFan22/CLAUDE.md`). Follow-up: check whether `[WARN-WELCOME-SELF-IN-ROOM]` still fires in production output.log; if gone, close that thread.

## Upstream style-guide sweep — remaining Markdown files (surveyed 2026-07-18)

PR #3794 review (pljones): Client/Server/Directory capitalised when referring to Jamulus in that role, per https://jamulus.io/contribute/Style-and-Tone; "probably applies to the other docs as well." JAMULUS_PROTOCOL.md done (`9e5408c3`/`18b248fb` pushed to `enhance-protocol-doc` 2026-07-18; all three review comments addressed). Remaining violations on upstream `main`, for one small follow-up docs PR after #3794 settles (offer it on the PR thread first):

- `README.md:10` — "Jamulus server/client… each client" (4 instances, all qualify)
- `CONTRIBUTING.md:19` — "communicate with the client and server"
- `SECURITY.md:35` — "Servers have a limited number of slots for clients"
- `COMPILING.md` — ~5 prose instances ("headless server build", "running the server", "starts server by default"); the `jamulus`/`.\deploy` *directory* instances are file-system, correct as-is
- `docs/JSON-RPC.md` — 103 lowercase instances, ~40–50 real after filtering. Traps: "JSON-RPC server" is NOT a Jamulus Server (stays lowercase); JSON keys/method names/code blocks untouched; `recordingDirectory` prose = file-system directory. Qualifying: "running as a server or client" (l.100), "servers in a directory" (l.227), table descriptions ("The server name" etc.)
- Clean already: docs/TRANSLATING.md, docs/README.md, src/sound/README.md, .github templates. `libs/opus` vendored — never touch.

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

**Zero-friction onramp — growth idea (2026-07-20):** pitch is "get online with a friend now, no latency, studio quality" — a connect-code flow targeted at people who already own audio hardware but aren't Jamulus users yet, sourced via low-budget paid ads on terms like "alternative to JamKazam" (a proxy for active hardware-havers, not cold traffic), pitch evolved by A/B test. Two prerequisites before spending any ad money:
- **Windows SmartScreen/AV friction**: an unsigned binary gets flagged as possible malware on first run — that warning dialog kills conversion on paid traffic even though organic/referral users tolerate it. Needs a code-signing certificate (EV cert skips the reputation-building period a plain OV cert requires) before this campaign, not after.
- **Install test harness**: paid traffic converting into a broken install (wrong audio driver, unusual interface, Windows version quirk) is unrecoverable ad-spend + reputation damage, unlike an organic user who'll just retry or ask in chat. Need a harness that exercises install across hardware/software combinations (audio interface variety, Windows versions, clean machine vs. existing Jamulus already installed) before any paid push — not designed yet.

Ties into the click-to-join bridge above (same binary, same web-app integration point) and the binary palette rollout below (same cert need).

**Cert cost/eligibility check (2026-07-20):** Apple Developer ID does NOT require a business entity — individual account, $99/yr, verified against personal identity, notarization included, no SmartScreen-style reputation ramp on Gatekeeper. Windows is the harder side: most traditional CAs (DigiCert/Sectigo/GlobalSign) now require a registered legal business for OV/EV certs (tightened since the 2023 hardware-key rule); the one open question is whether Microsoft's Trusted Signing service's individual-identity-verification path (via Entra Verified ID, ~$10/mo) still avoids the business requirement — unconfirmed, check current docs before assuming either way. **Decision leaning:** ship signed+notarized on Mac first (cheap, no business needed); for Windows, either eat the SmartScreen warning and let OV reputation build organically, or hold the whole Windows onramp push until Trusted Signing's individual path is confirmed viable. Not worth an LLC + EV cert for a low-budget experiment.

Correction on Microsoft's false-positive process: WDSI file submission (microsoft.com/wdsi/filesubmission) only fast-tracks the case where Defender/AV actively flags the binary as a malware signature match (24–72hr review). It does NOT fast-track the generic SmartScreen "unrecognized publisher" warning — that one is pure reputation-by-install-volume with no submission queue, so for a low-download build it stays slow regardless.

**Priority call (2026-07-20): existing-users-first, paid-onramp deferred.** Cert cost/friction makes the "reach brand-new Windows users via paid ads" plan expensive and not worth pursuing yet. The cheap, high-value move instead: pitch click-to-join (once #3806 lands) to people who **already have Jamulus installed** — they've already cleared SmartScreen once, so zero new install friction and no cert dependency at all. Distribution channel: the existing JamFan22 dynamic welcome-message mechanism (see Announcement, above) — pitch click-to-join / invite-a-friend features directly in welcome messages to current fleet users, same delivery path already planned for the Windows client announcement.

**Browser-JSON-RPC feature brainstorm (2026-07-20), once the WS bridge + #3806 exist** — check `docs/JSON-RPC.md` for what's actually already exposed before assuming any of these beyond connect/disconnect:
- Live status widget in the web page (connection state/room/ping via WS push, no polling)
- Deep-link "join this room" — generalizes click-to-join to any shared link, not just a JamFan22 button
- Web-based mixer — per-channel fader/mute in-browser, useful from a phone while the real client runs headless
- Chat relay into the web page, reusing existing chat-reporter plumbing
- One-click diagnostics — button posts current connection state + recent RPC errors to cut down support back-and-forth
- Auto-set identity on connect (name/instrument/country from JamFan22 profile) — needs checking whether a `setProfile`-equivalent RPC exists

**Rollout sequencing (2026-07-20), confirmed:** ship bridge → verify in production → write A/B-tested multilingual welcome pitch, in that order. Don't write the pitch copy before the feature is confirmed working — advertise what's already shipped, not what's on a draft PR.

**V1 client scope (2026-07-20):** deliberately minimal — browser paired with the binary (click-to-join bridge, auto-launch, modified icon) + client-side song-artist harvesting. Nothing else; explicitly deferred the browser-RPC feature list above to V2. Two prerequisites called out before this can go out in a wide welcome-message pitch:
1. `/chat-url-client` rate-limiting/IP-gating (see below) — V1 adds a second chat-parsing feature (song harvesting) hitting the same untrusted-input surface, making this hardening non-optional rather than nice-to-have.
2. Two existing loose ends in the same code path: chat-reporter URL detection isn't fully wired in the GUI client build yet (see Wire-up remaining, above), and the video-link false-positive bug (above) — both would be visibly embarrassing once URL sharing is the thing being pitched.

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
