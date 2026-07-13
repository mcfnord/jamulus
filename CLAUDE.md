# Jamulus Fork + Fleet — Operating Manual

Fork of [jamulussoftware/jamulus](https://github.com/jamulussoftware/jamulus). `main` tracks upstream exactly; all custom work lives on `jamfan` (rebased onto `main`; history rewrite acceptable). Not intended for upstream except explicitly noted PRs. Open work: `TODO.md` here and `/root/TODO.md`.

## Building

Qt 5.15 required.

```bash
sudo apt-get install -y build-essential qtbase5-dev qt5-qmake qtmultimedia5-dev \
    qttools5-dev-tools libjack-jackd2-dev libqt5websockets5-dev
qmake "CONFIG+=headless" Jamulus.pro
make -j$(nproc)
```

Binary: `./Jamulus`. Server mode: `--server --nogui`. Feature logging:
`QT_LOGGING_RULES="jamulus.chatreporter=true;jamulus.centraldefense=true"`.

**Runtime dependency (binaries built after 2026-07-04): `libqt5websockets5`.** The fleet-rpc-channel work links QWebSockets. A host without it crash-loops with `libQt5WebSockets.so.5: cannot open shared object file` (this took Milan down for 655 restarts on 2026-07-07). provision.sh must install it; every deploy must end with an `ldd` check.

## Custom features (all on `jamfan`)

- **central-defense** (`src/centraldefense.cpp`): per-IP allow/deny via `GET https://jamulus.live/ip-allowed/{ip}`. Gates the UDP audio path before `PutAudioData`. **Audio-thread law**: `shouldAllow()` never blocks — cache hit returns immediately; cache miss fails open and queues an async lookup (`QMetaObject::invokeMethod`, QueuedConnection). The pre-fix synchronous `QEventLoop::exec()` in this path silenced whole servers for up to 2s per cache expiry (fixed 2026-06-24). TTLs: blocked 5 min, allowed 20 s, error → allowed 20 s. Local allowlist `/etc/jamulus/ip-allowlist.txt` checked first.
- **chat-reporter** (`src/chatreporter.cpp`): extracts URLs from chat → POST `/chat-url-server` (server builds) or `/chat-url-client` (client builds); handles `/stream` command; `reportClientInfo` computes GUID = MD5(name + phpCountryName + phpInstrumentName) and GETs `/player-identified/...`. Server builds fetch patterns from `jamulus.live/chat-patterns.txt` hourly; client builds use hardcoded `kPatterns[]` (line ~40). **The two lists must stay identical — add domains to both.**
- **earlier-join-notification**: connection notification at the earliest socket step (`socket.cpp`, serverlogging).
- **make_welcome**: Debian postinst creates `/etc/jamulus/welcome.html`.
- **recording-banner-api**: `jamulusserver/setRecordingBanner {"active":bool}` — shows the red RECORDING banner without recording (`m_bExternalRecordingBanner`).

**Standing rules:**
- **GUID trap**: never use `CInstPictures::GetName()` for GUID computation — diverges from `phpInstrumentName` at indices 0, 1, 26, 27. JamFan22's `_instrumentNames[]` and `sampler.py` on 137.184.43.255 are the correct tables.
- **rawaudio stays out**: the PCM/MAX probe (IDs 1036/1037) was fully reverted (`dd25a580`) — no MAX-capable server anywhere, especially Trio. `AQ_RAW` remains unreachable; keep it that way. (Historical note: that revert collaterally broke 1028 handling for ~32h in June 2026; 1028 is restored and is unrelated to rawaudio.)
- **`checkAndLookup` stays in `OnNewConnection`** — it is the only gate against silent clients (audio-path gate never fires for them). The `QObject: Cannot create children…` thread warning it produces is non-fatal; the proper fix is dispatching it to the main thread via QueuedConnection, same pattern as `shouldAllow()`.

## Fleet reference

Config: `fleet.json` (names, hosts, users, `arch` [default x86_64], `os`, `gcc13_fix`, service names, ports, rpcports, `dormant`, `instance_id`, `cc`, `maxclients`). Standard server: service `jamulus-headless.service`, binary `/usr/bin/jamulus-jamfan`, UDP port 22224, JSON-RPC `--jsonrpcport 9999 --jsonrpcsecretfile /secret.txt --jsonrpcbindip 0.0.0.0` (jazz sidecars: 9998; rock: 9997). RPC ports firewalled to lounge (147.182.199.22) + jamulus.live (134.199.209.51); the RPC secret lives in `/secret.txt` on each host and is supplied to local tooling via `~/.jamulus-rpc-secret` (kept out of source control).

- **Deploy**: `./deploy.sh <name|all>` (builds, routes by arch/OS, syncs fleet-server-ips.txt to jamulus.live).
- **Membership change without deploy**: `./sync-fleet-ips.sh` (also regenerates `fleet-rpc-ports.txt` — never edit either file by hand).
- **Provision** (Ubuntu 24.04): `./provisioning/provision.sh <host> [user]`, then the checklist below.
- **Dormant IPs are dynamic**: authoritative current IP is `/root/dormant-ip-cache.json` on jamulus.live, keyed by `instance_id`. fleet.json `host` goes stale for dormant entries after every restart.
- **Swap is forbidden on fleet hosts** (latency spikes). Sole exception: transient swap for Harry's Docker rebuild, removed after.

### Deploy verification (run after every deploy, per host)

```bash
ssh <user>@<ip> 'file /usr/bin/jamulus-jamfan; uname -m;
  ldd /usr/bin/jamulus-jamfan | grep -c "not found";
  systemctl list-units "jamulus-*" --no-legend --plain'
```
Pass = arch matches, 0 missing libs, all services `active running` (re-check after 60s: restart counter stable), `--version` shows the new revision. The 2026-07-07 dual outage (ARM binary on x86_64 Maple; missing websockets lib on Milan) is exactly what this catches. `/deploy` skill automates it.

### ABI / architecture pitfalls

- **Never rsync the local x86-64 `Jamulus` binary to ARM hosts** — always `--exclude='Jamulus'` when rsyncing sources.
- **Never mix OS groups**: Ubuntu 24.04 binary on 22.04 → `GLIBCXX_3.4.32 not found` crash. Ubuntu 26.04 exists (several dormant hosts run it — verified live 2026-07-07) and runs 24.04-built binaries fine; the reverse does not hold. Rising (22.04, no `gcc13_fix`) builds natively and is slated for replacement.
- **Cross-deploy cache**: first-built aarch64/u24 binary cached at `/tmp/jamfan-arm64-u24`, reused for `gcc13_fix: true` hosts in the same run; delete to force rebuild. deploy.sh compares cached rev to current and rebuilds when stale. Because the rev counter increments per run, a single-target run against a `skip_native_build` host (San Jose) always sees the cache as stale and skips — pin `jamfan-rev` to (cached rev − 1) first so the run's rev matches the cache. Same trick skips needless local x86 rebuilds when `./Jamulus` already holds the current source (sources unchanged since its build).
- **GCC 13 moc fix** (Ubuntu 24.04): deploy.sh applies it automatically for `gcc13_fix: true`. Manual: build `release/moc_predefs.h`, append `#undef _GLIBCXX_VISIBILITY` / `#define _GLIBCXX_VISIBILITY(V)`, then `make`.
- **Freiheit/Jazzkeller share a host** — native builds there cause dropouts; deploy.sh pre-flight blocks builds when clients are present.

### Provisioning checklist (after provision.sh)

1. Console: append the ed25519 pubkey (`root@test-jamulus-jamfan`) to `~/.ssh/authorized_keys`.
2. `./provisioning/provision.sh <host> [user]`.
3. `cat ~/.jamulus-rpc-secret > /secret.txt && chmod 644 /secret.txt` (must be world-readable — the `jamulus` user reads it; secret value kept out of source control).
4. Extra instances: one service file each (`--port`, `--jsonrpcport`, `--serverinfo`, `-u <maxclients>`).
5. `apt-get install -y libqt5websockets5` (until provision.sh includes it).
6. iptables: RPC ports 9997–9999 restricted to lounge + jamulus.live; `iptables-persistent` + `netfilter-persistent save`.
7. UDP buffer: `net.core.rmem_max=4194304 net.core.rmem_default=4194304` in `/etc/sysctl.d/99-jamulus.conf` (prevents dropouts under load).
8. `systemctl daemon-reload && systemctl enable --now <services>`.
9. Add all instances to fleet.json (`"lang": "Dutch"` for NL, etc.), then `./sync-fleet-ips.sh`.
10. Verify against the provisioning quality bar in `/root/CLAUDE.md` (directory listing, `[IP-ALLOWED]`, welcome delivered).

**Oracle Linux**: firewalld runs by default — need OCI security-list rules AND `firewall-cmd` (`--add-port=22224/udp`; rich rules for RPC TCP). Done on Turin.
**Unreachable ASNs**: AS58955 (Bangmod, Bangkok) and AS197540 (netcup) cannot be provisioned on.
**Docker server (Harry's, 35.89.21.63)**: image built from `/tmp/harry-docker/` (cleared on reboot — re-stage Dockerfile + binary first; Dockerfile master copy `/tmp/harry-Dockerfile` locally). Image must include `ca-certificates` or CentralDefense HTTPS fails. RAM-constrained rebuild: create 1G swap → stop service → stage → `docker build -t jamulus-harry .` → start service → remove swap.

### Dormant instances

Monitor: `dormant-monitor.py` runs on jamulus.live as `dormant-monitor.service` (every 20 min, boto3 start/stop by geographic demand). Live table: `ssh root@jamulus.live 'grep "\[DORMANT\]" /root/dormant-monitor.log | tail -20'`. Instance list AND per-instance thresholds live in `/root/dormant-instances.json` on jamulus.live (fields: name, instance_id, region, lat/lon, threshold, stop_streak, fleet_entries) — edit the JSON, then `systemctl restart dormant-monitor`. (The old `/root/dormant-thresholds.json` and in-script INSTANCES are gone — verified 2026-07-10.)

Instance names are geographic, not server names: Oregon→Portland, Thailand→สวัสดีครับ, Milan→La Scala, Formosa/Taiwan→女巫店/藍調/The Wall (box name Formosa; 女巫店 ex-Riverside, 藍調 ex-Legacy — renamed 2026-07-12), San Jose→No Way/Studio F, Montreal→Maple, Paris→Louvre, São Paulo→Paulista, Singapore→Esplanade, Spain→Alhambra, Mexico City→Garibaldi, Calgary→Stampede, Ohio→Agora, Virginia→The National, Hong Kong→Wan Chai, Zurich→Tonhalle/Moods.

Demand table (`ssh root@jamulus.live 'python3 /tmp/demand_scores.py'`) — always render as markdown with a % column (score/threshold × 100), **ON** bold for running:

| City | Score | Threshold | % | Status |
|------|------:|----------:|--:|--------|
| Oregon | 0.0252 | 0.02 | 126% | **ON** |

`demand_scores.py` reads the same `/root/dormant-instances.json` — no separate sync needed.

### gjstress — capacity stress tool

Binary: `/root/gojam/gjstress` (local), `/usr/local/bin/gjstress` (lounge). Source `mcfnord/gojam cmd/gjstress/`; build with `CGO_CFLAGS="-I/usr/include/opus" go build ./cmd/gjstress/`.

**Run from the lounge only** (central command is CentralDefense-blocked). AWS/OCI need `--directory` hole-punch; Linode/DO don't.

```bash
ssh root@147.182.199.22 'gjstress --target <ip:port> --bots N --duration 2m --ramp 10s --tone [--directory anygenre1.jamulus.io:22124]'
```

`rx%` (expected ~375 packets/10s per bot): ok ≥80%, warn 50–79%, DEGRADED <50%. Before testing: raise the `-u` cap (sed + restart), restore after; check `free -m` on small hosts.

### Fleet analysis scripts (local, run against jamulus.live data)

- `/root/fleet_perf.py` — per-instance comparison (aggregate all servers on one IP; metrics: unique GUIDs with audible>0, minutes with audible ≥2 — bot-resistant). Run: `scp /root/fleet_perf.py /root/jamulus/fleet.json root@jamulus.live:/tmp/ && ssh root@jamulus.live python3 /tmp/fleet_perf.py`.
- `/root/fleet-expansion-targets.py` — primary expansion-targeting tool: ranks non-fleet servers by unique GUIDs + active minutes (30d census), cross-references names from `/tmp/directory-*.json` on 137.184.43.255. Same scp+run pattern.
- `/root/blocked_census.py` (long-running local poller → `~/blocked_census.csv`) + `/root/census_summary.py` (viewer) — covers servers that block the harvester.
- Geo/ASN of a candidate IP: `curl "http://ip-api.com/json/<ip>?fields=query,country,city,org,as"`. Blocked-server capacity: `/var/www/html/blocked-servers.txt` on 137.184.43.255.

## Debugging deployments

When a fix "isn't working": first `ssh <host> 'ls -la /usr/bin/jamulus-jamfan'` vs `git -C /root/jamulus log -1 --format="%ci" <commit>`. Binary older than commit → deploy, don't debug. Then run the deploy verification block above — arch, libs, and restart-loop state explain most "mysteries."

## JSON-RPC

`./rpc.sh <server-name> <method> [json-params]` from this directory — routes through the lounge. Example: `./rpc.sh 'Hot Texas!' jamulusserver/getClients`.

**rpc.sh bug**: params containing JSON booleans/numbers get mangled through SSH. Workaround — inline Python on the lounge:

```bash
ssh root@147.182.199.22 'python3 -c "
import socket, json
s = socket.socket(); s.connect((\"<fleet-ip>\", 9999)); s.settimeout(5)
def rpc(m, p={}):
    s.sendall((json.dumps({\"id\":1,\"jsonrpc\":\"2.0\",\"method\":m,\"params\":p})+\"\n\").encode())
    return json.loads(s.recv(4096))
rpc(\"jamulus/apiAuth\", {\"secret\": open(os.path.expanduser(\"~/.jamulus-rpc-secret\")).read().strip()})
print(json.dumps(rpc(\"<method>\", {\"active\": False}), indent=2))
s.close()"'
```

## Protocol notes

- **1028 → 1015** (channel levels): fleet-only (our build). Never assume it on non-fleet servers.
- **1014 → 1013** (client list): works on all standard Jamulus servers, connectionless. 1013's channel slot = the index in 1015 nibbles — correlate by slot for per-client levels.
- **CLM_SEND_EMPTY_MESSAGE**: directory→server hole-punch instruction; server→client packets — they do NOT contaminate the correlation engine's client→server timing signal.

## Windows client build

GitHub Actions: `git push origin jamfan:autobuild-jamfan`, download artifact, delete the remote branch. Rollout plan and URL-abuse prerequisites: `TODO.md`.
