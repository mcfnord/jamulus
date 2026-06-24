#!/usr/bin/env bash
# Usage: ./deploy.sh [<server-name-or-ip> | all]
# x86-64 servers: builds locally, scps binary, restarts.
# aarch64 servers: rsyncs sources, builds natively on host, restarts.

set -euo pipefail
cd "$(dirname "$0")"

FLEET="fleet.json"
BINARY="./Jamulus"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <server-name-or-ip | all>"
  echo
  echo "Known servers:"
  python3 -c "
import json
for s in json.load(open('$FLEET')):
    arch = s.get('arch', 'x86_64')
    print(f\"  {s['name']:22s}  {s['host']:18s}  ({s['user']})  [{arch}]\")
"
  exit 1
fi

TARGET="$1"

# Increment build rev and format as two hex digits (wraps at 256)
REV_FILE="jamfan-rev"
REV=$(( $(cat "$REV_FILE" 2>/dev/null || echo "0") + 1 ))
printf '%d\n' "$REV" > "$REV_FILE"
HEX_REV=$(printf '%02x' "$(( REV % 256 ))")
JAMFAN_VERSION="3.12.1-JAMFAN-${HEX_REV}"

# Output tab-separated: host user service arch gcc13_fix swapon make_jobs qmake_cmd name
# name is last so spaces in server names are handled correctly by bash read.
readarray -t PAIRS < <(python3 -c "
import json, sys
fleet = json.load(open('$FLEET'))
target = sys.argv[1]
servers = fleet if target == 'all' else [
    s for s in fleet if s['name'].lower() == target.lower() or s['host'] == target
]
if not servers:
    print(f'No server matching: {target}', file=sys.stderr)
    sys.exit(1)
for s in servers:
    arch     = s.get('arch', 'x86_64')
    service  = s.get('service', 'jamulus-headless')
    gcc13    = '1' if s.get('gcc13_fix', False) else '0'
    swapon   = s.get('swapon', '-')
    jobs     = str(s.get('make_jobs', 0))   # 0 = use nproc on remote
    qmake    = s.get('qmake_cmd', 'qmake')
    print('\t'.join([s['host'], s['user'], service, arch, gcc13, swapon, jobs, qmake, s['name']]))
" "$TARGET")

echo "==> Build rev: $JAMFAN_VERSION"

# Build x86-64 binary locally once if any x86-64 targets in this run
HAS_X86=$(python3 -c "
import json, sys
fleet = json.load(open('$FLEET'))
target = sys.argv[1]
servers = fleet if target == 'all' else [s for s in fleet if s['name'].lower() == target.lower() or s['host'] == target]
print('1' if any(s.get('arch', 'x86_64') == 'x86_64' for s in servers) else '0')
" "$TARGET")

if [[ "$HAS_X86" == "1" ]]; then
    echo "==> Building x86-64 (${JAMFAN_VERSION})"
    qmake "CONFIG+=headless" "JAMFAN_REV=${HEX_REV}" Jamulus.pro > /dev/null
    current_ver=$(strings ./Jamulus 2>/dev/null | grep -o 'JAMFAN-[0-9a-f][0-9a-f]' | head -1 || echo "")
    if [[ "$current_ver" != "JAMFAN-${HEX_REV}" ]]; then
        echo "    version changed (${current_ver} → JAMFAN-${HEX_REV}), clean rebuild"
        make clean > /dev/null 2>&1 || true
    fi
    make -j$(nproc)
    echo "    x86-64 build done."
fi

declare -A BUILT_HOSTS
ARM64_U24_BINARY=""   # path to cached ubuntu24/aarch64 binary for cross-deploy

for pair in "${PAIRS[@]}"; do
  IFS=$'\t' read -r host user service arch gcc13 swapon jobs qmake name <<< "$pair"
  echo "==> $name ($user@$host) [$service]"
  ssh-keyscan -H "$host" >> ~/.ssh/known_hosts 2>/dev/null

  if [[ "$arch" == "aarch64" ]]; then
    if [[ -z "${BUILT_HOSTS[$host]:-}" ]]; then
      if [[ "$gcc13" == "1" && -n "$ARM64_U24_BINARY" ]]; then
        # Cross-deploy: a binary built on any ubuntu24/aarch64 host runs on all others.
        echo "    cross-deploying ubuntu24/aarch64 binary (no build needed) ..."
        scp -i ~/.ssh/id_ed25519 "$ARM64_U24_BINARY" "$user@$host:/tmp/jamulus-jamfan"
        ssh -i ~/.ssh/id_ed25519 "$user@$host" \
            "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan && sudo chmod +x /usr/bin/jamulus-jamfan"
        BUILT_HOSTS[$host]=1
      else
        # Pre-flight: refuse to build if any service on this host has active clients.
        # A native build saturates the CPU and causes audio dropouts for live sessions.
        # Checks all RPC ports on this host, not just the service being deployed.
        rpc_ports=$(python3 -c "
import json
fleet = json.load(open('$FLEET'))
print(','.join(str(s.get('rpcport', 9999)) for s in fleet if s['host'] == '$host'))
")
        active_check=$(ssh -i ~/.ssh/id_ed25519 "$user@$host" python3 <<PYCHECK || true
import socket, json
active = []
for port in [int(p) for p in '${rpc_ports}'.split(',') if p]:
    try:
        s = socket.socket(); s.connect(('127.0.0.1', port)); s.settimeout(3)
        secret = open('/secret.txt').read().strip()
        def rpc(m, p={}):
            s.sendall((json.dumps({'id':1,'jsonrpc':'2.0','method':m,'params':p})+'\n').encode())
            return json.loads(s.recv(4096))
        rpc('jamulus/apiAuth', {'secret': secret})
        n = len(rpc('jamulusserver/getClients').get('result',{}).get('clients',[]))
        if n > 0:
            active.append(f'port {port}: {n} client(s)')
        s.close()
    except:
        pass
print('\n'.join(active))
PYCHECK
)
        if [[ -n "$active_check" ]]; then
          echo "    SKIPPED $host — active sessions detected, will not build while live:"
          echo "$active_check" | sed 's/^/      /'
          echo "    Re-run deploy for $name when quiet."
          continue
        fi

        BUILT_HOSTS[$host]=1
        echo "    rsyncing sources to $host ..."
        rsync -a \
          --exclude='.git' --exclude='*.o' --exclude='*.lo' --exclude='*.a' \
          --exclude='release/' --exclude='Jamulus' \
          --exclude='libs/opus/.libs/' --exclude='libs/opus/autom4te.cache/' \
          -e "ssh -i ~/.ssh/id_ed25519" \
          ./ "$user@$host:/tmp/jamulus-build/"
        echo "    building natively on $host (takes a few minutes) ..."
        # Generate and pipe a build script; Python handles escaping cleanly.
        python3 - "$gcc13" "$swapon" "$jobs" "$qmake" "$HEX_REV" <<'PYEOF' | ssh -i ~/.ssh/id_ed25519 "$user@$host" bash
import sys
gcc13, swapon, jobs, qmake, hex_rev = sys.argv[1:]
lines = ['set -euo pipefail', 'cd /tmp/jamulus-build']
if swapon != '-':
    lines.append(f'sudo swapon {swapon} 2>/dev/null || true')
lines.append(f'{qmake} "CONFIG+=headless" "JAMFAN_REV={hex_rev}" Jamulus.pro')
if gcc13 == '1':
    # GCC 13 / Qt5 moc can't parse _GLIBCXX_VISIBILITY; strip it from moc_predefs.h.
    lines += [
        'mkdir -p release',
        'make -f Makefile.Release release/moc_predefs.h',
        r'printf "\n#undef _GLIBCXX_VISIBILITY\n#define _GLIBCXX_VISIBILITY(V)\n" >> release/moc_predefs.h',
    ]
lines.append('make -j1' if jobs == '1' else 'make -j$(nproc)')
lines += ['sudo mv Jamulus /usr/bin/jamulus-jamfan', 'sudo chmod +x /usr/bin/jamulus-jamfan']
print('\n'.join(lines))
PYEOF
        if [[ "$gcc13" == "1" ]]; then
          echo "    caching ubuntu24/aarch64 binary for cross-deploy ..."
          scp -i ~/.ssh/id_ed25519 "$user@$host:/usr/bin/jamulus-jamfan" /tmp/jamfan-arm64-u24
          ARM64_U24_BINARY="/tmp/jamfan-arm64-u24"
        fi
      fi
    fi
    ssh -i ~/.ssh/id_ed25519 "$user@$host" "sudo systemctl restart $service"
    echo "    done."

  else
    scp -i ~/.ssh/id_ed25519 "$BINARY" "$user@$host:/tmp/jamulus-jamfan"
    ssh -i ~/.ssh/id_ed25519 "$user@$host" \
      "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan \
       && sudo chmod +x /usr/bin/jamulus-jamfan \
       && sudo systemctl restart $service"
    echo "    done."
  fi
done

# Keep jamfan22's fleet-server-ips.txt in sync with fleet.json
# 147.182.199.22 = lounge — trusted caller for /ip-allowed, not a game server
{ python3 -c "
import json
fleet = json.load(open('$FLEET'))
for s in fleet:
    ip = s['host']
    port = s.get('port', 22224)
    rpcport = s.get('rpcport', 9999)
    dirhost = 'jazz.jamulus.io' if s.get('service') == 'jamulus-jazz' else 'anygenre1.jamulus.io'
    dirport = 22324 if s.get('service') == 'jamulus-jazz' else 22124
    print(f'{ip}:{port}:{rpcport}:{dirhost}:{dirport}')
"
echo "147.182.199.22"
echo "24.199.107.192"
for p in 22121 22122 22123 22124 22125 22126 22127; do echo "24.199.107.192:$p"; done
} | ssh -i ~/.ssh/id_ed25519 root@jamulus.live \
    'cat > /root/JamFan22/JamFan22/data/fleet-server-ips.txt'
echo "Fleet IPs synced to jamfan22."
