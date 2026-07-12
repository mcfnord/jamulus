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
FORCE=0
for arg in "$@"; do [[ "$arg" == "--force" ]] && FORCE=1; done

# Rsyslog filter to suppress CentralDefense threading spam (fills disk on idle servers)
RSYSLOG_B64=$(printf ':msg, contains, "QObject: Cannot create children for a parent that is in a different thread" stop\n' | base64 -w0)
APPLY_RSYSLOG="echo ${RSYSLOG_B64} | base64 -d | sudo tee /etc/rsyslog.d/90-suppress-jamulus.conf > /dev/null && sudo systemctl reload rsyslog 2>/dev/null || true"

# Fetch current IPs for dormant instances (instance_id → ip) from jamulus.live.
# Falls back to fleet.json host field if cache is unavailable.
DORMANT_IPS=$(ssh -i ~/.ssh/id_ed25519 root@jamulus.live 'cat /root/dormant-ip-cache.json 2>/dev/null || echo "{}"' 2>/dev/null || echo '{}')

# Increment build rev and format as two hex digits (wraps at 256)
REV_FILE="jamfan-rev"
REV=$(( $(cat "$REV_FILE" 2>/dev/null || echo "0") + 1 ))
printf '%d\n' "$REV" > "$REV_FILE"
HEX_REV=$(printf '%02x' "$(( REV % 256 ))")
JAMFAN_VERSION="3.12.1-JAMFAN-${HEX_REV}"

# Output tab-separated: host user service arch gcc13_fix swapon make_jobs qmake_cmd name
# name is last so spaces in server names are handled correctly by bash read.
readarray -t PAIRS < <(DORMANT_IPS="$DORMANT_IPS" python3 -c "
import json, sys, os
fleet = json.load(open('$FLEET'))
dormant_ips = json.loads(os.environ.get('DORMANT_IPS', '{}'))
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
    qmake       = s.get('qmake_cmd', 'qmake')
    rpcport     = str(s.get('rpcport', 9999))
    secret_file = s.get('secret_file', '/secret.txt')
    skip_nb     = '1' if s.get('skip_native_build', False) else '0'
    iid  = s.get('instance_id', '')
    if s.get('dormant') and iid and iid not in dormant_ips:
        import sys as _sys
        print(f'SKIP_DORMANT\t{s[\"name\"]}', file=_sys.stderr)
        continue
    host = dormant_ips.get(iid, s['host']) if iid else s['host']
    print('\t'.join([host, s['user'], service, arch, gcc13, swapon, jobs, qmake, rpcport, secret_file, skip_nb, s['name']]))
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
    existing_ver=$(strings "$BINARY" 2>/dev/null | grep -o 'JAMFAN-[0-9a-f][0-9a-f]' | head -1 || echo "")
    if [[ "$existing_ver" == "JAMFAN-${HEX_REV}" ]]; then
        echo "==> Using existing x86-64 binary ($existing_ver)"
    else
        echo "==> Building x86-64 (${JAMFAN_VERSION}) — was: ${existing_ver:-missing}"
        qmake "CONFIG+=headless" "JAMFAN_REV=${HEX_REV}" Jamulus.pro > /dev/null
        make clean > /dev/null 2>&1 || true
        make -j$(nproc)
        echo "    x86-64 build done."
    fi
fi

declare -A BUILT_HOSTS
ARM64_U24_BINARY=""   # path to cached ubuntu24/aarch64 binary for cross-deploy
if [[ -f /tmp/jamfan-arm64-u24 ]]; then
    CACHED_VER=$(strings /tmp/jamfan-arm64-u24 2>/dev/null | grep -o 'JAMFAN-[0-9a-f][0-9a-f]' | head -1 || echo "?")
    if [[ "$CACHED_VER" == "JAMFAN-${HEX_REV}" ]]; then
        ARM64_U24_BINARY="/tmp/jamfan-arm64-u24"
        echo "==> Using cached aarch64 binary (${CACHED_VER}) — delete /tmp/jamfan-arm64-u24 to force rebuild"
    else
        echo "==> Cached aarch64 binary is ${CACHED_VER}, current rev is JAMFAN-${HEX_REV} — forcing native rebuild"
    fi
fi

for pair in "${PAIRS[@]}"; do
  IFS=$'\t' read -r host user service arch gcc13 swapon jobs qmake rpcport secret_file skip_nb name <<< "$pair"
  echo "==> $name ($user@$host) [$service]"

  skip=$(python3 -c "import json; fleet=json.load(open('$FLEET')); s=next((x for x in fleet if x['name']==\"$name\"),{}); print('1' if s.get('skip') else '0')")
  if [[ "$skip" == "1" ]]; then
    echo "    SKIPPED (skip=true in fleet.json)"
    continue
  fi

  ssh-keyscan -H "$host" >> ~/.ssh/known_hosts 2>/dev/null

  # For dormant instances, verify SSH is reachable before attempting deploy.
  # Stale cache entries (instance stopped after cache was read) should skip, not abort.
  is_dormant=$(python3 -c "import json; fleet=json.load(open('$FLEET')); s=next((x for x in fleet if x['name']==\"$name\"),{}); print('1' if s.get('dormant') else '0')")
  if [[ "$is_dormant" == "1" ]]; then
    if ! ssh -i ~/.ssh/id_ed25519 -o ConnectTimeout=8 -o BatchMode=yes "$user@$host" 'echo ok' &>/dev/null; then
      echo "    SKIP_DORMANT $name — SSH unreachable at $host (stale cache entry)"
      continue
    fi
  fi

  # For Rock servers: generate and push the service file, set up iptables for the RPC port
  if [[ "$service" == "jamulus-rock" ]]; then
    svc_tmp=$(mktemp /tmp/jfan-rock.XXXXXX)
    python3 -c "
import sys, json
name, fleet_path = sys.argv[1], sys.argv[2]
fleet = json.load(open(fleet_path))
s = next((x for x in fleet if x.get('name') == name), {})
port      = s.get('port', 22225)
rpcport   = s.get('rpcport', 9998)
cc        = s.get('cc', 'US')
maxcli    = s.get('maxclients', 8)
group     = 'nobody' if s.get('os') == 'oracle-linux-9' else 'nogroup'
print(f'''[Unit]
Description=Jamulus Rock server - {name}
After=network.target
StartLimitIntervalSec=0

[Service]
User=jamulus
Group={group}
NoNewPrivileges=true
ProtectSystem=true
Nice=-20
IOSchedulingClass=realtime
IOSchedulingPriority=0
ExecStart=/bin/sh -c 'exec /usr/bin/jamulus-jamfan --nogui --server --port {port} --directoryserver rock.jamulus.io:22424 --serverinfo \"{name};;{cc}\" -u {maxcli} --jsonrpcport {rpcport} --jsonrpcsecretfile /secret.txt --jsonrpcbindip 0.0.0.0'
Restart=on-failure
RestartSec=30
SyslogIdentifier=jamulus-rock

[Install]
WantedBy=multi-user.target''')
" "$name" "$FLEET" > "$svc_tmp"
    if scp -i ~/.ssh/id_ed25519 -o ConnectTimeout=30 -o BatchMode=yes "$svc_tmp" "$user@$host:/tmp/jamulus-rock.service" 2>/dev/null; then
      ssh -i ~/.ssh/id_ed25519 -o ConnectTimeout=30 "$user@$host" "
        sudo mv /tmp/jamulus-rock.service /etc/systemd/system/jamulus-rock.service
        sudo systemctl daemon-reload
        sudo systemctl enable jamulus-rock 2>/dev/null || true
        if ! sudo iptables -C INPUT -p tcp --dport ${rpcport} -j DROP 2>/dev/null; then
          sudo iptables -I INPUT 1 -s 147.182.199.22 -p tcp --dport ${rpcport} -j ACCEPT 2>/dev/null || true
          sudo iptables -I INPUT 2 -s 134.199.209.51 -p tcp --dport ${rpcport} -j ACCEPT 2>/dev/null || true
          sudo iptables -I INPUT 3 -s 127.0.0.1 -p tcp --dport ${rpcport} -j ACCEPT 2>/dev/null || true
          sudo iptables -A INPUT -p tcp --dport ${rpcport} -j DROP 2>/dev/null || true
          sudo netfilter-persistent save 2>/dev/null || sudo iptables-save | sudo tee /etc/iptables/rules.v4 >/dev/null 2>/dev/null || true
        fi
      " 2>/dev/null && echo "    service file pushed." || echo "    service file install failed (non-fatal)"
    else
      echo "    service file SCP failed (non-fatal)"
    fi
    rm -f "$svc_tmp"
  fi

  if [[ "$arch" == "aarch64" ]]; then
    if [[ -z "${BUILT_HOSTS[$host]:-}" ]]; then
      if [[ "$gcc13" == "1" && -n "$ARM64_U24_BINARY" ]]; then
        # Cross-deploy: a binary built on any ubuntu24/aarch64 host runs on all others.
        echo "    cross-deploying ubuntu24/aarch64 binary (no build needed) ..."
        if ! scp -i ~/.ssh/id_ed25519 -o ConnectTimeout=30 -o BatchMode=yes "$ARM64_U24_BINARY" "$user@$host:/tmp/jamulus-jamfan" 2>/dev/null; then
          [[ "$is_dormant" == "1" ]] && { echo "    SKIP_DORMANT $name — SCP failed (stopped mid-flight)"; continue; }
          scp -i ~/.ssh/id_ed25519 "$ARM64_U24_BINARY" "$user@$host:/tmp/jamulus-jamfan"
        fi
        if ! ssh -i ~/.ssh/id_ed25519 -o ConnectTimeout=30 -o BatchMode=yes "$user@$host" \
            "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan && sudo chmod +x /usr/bin/jamulus-jamfan && $APPLY_RSYSLOG" 2>/dev/null; then
          [[ "$is_dormant" == "1" ]] && { echo "    SKIP_DORMANT $name — SSH failed (stopped mid-flight)"; continue; }
          ssh -i ~/.ssh/id_ed25519 "$user@$host" \
              "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan && sudo chmod +x /usr/bin/jamulus-jamfan && $APPLY_RSYSLOG"
        fi
        BUILT_HOSTS[$host]=1
      else
        if [[ "$skip_nb" == "1" ]]; then
          echo "    SKIPPED $name — skip_native_build=true and no cached binary available; deploy another gcc13 host first"
          continue
        fi
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

        host_services=$(python3 -c "
import json
fleet = json.load(open('$FLEET'))
seen = []
for s in fleet:
    if s['host'] == '$host':
        svc = s.get('service', 'jamulus-headless')
        if svc not in seen: seen.append(svc)
print(' '.join(seen))
")
        echo "    stopping services on $host for build: $host_services ..."
        ssh -i ~/.ssh/id_ed25519 "$user@$host" "sudo systemctl stop $host_services" 2>/dev/null || true

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
lines += [
    'current_ver=$(strings /usr/bin/jamulus-jamfan 2>/dev/null | grep -o \'JAMFAN-[0-9a-f][0-9a-f]\' | head -1 || echo "")',
    f'if [[ "$current_ver" != "JAMFAN-{hex_rev}" ]]; then make clean > /dev/null 2>&1 || true; fi',
]
if gcc13 == '1':
    # GCC 13 / Qt5 moc can't parse _GLIBCXX_VISIBILITY; strip it from moc_predefs.h.
    lines += [
        'mkdir -p release',
        'make -f Makefile.Release release/moc_predefs.h',
        r'printf "\n#undef _GLIBCXX_VISIBILITY\n#define _GLIBCXX_VISIBILITY(V)\n" >> release/moc_predefs.h',
    ]
lines.append('make -j1' if jobs == '1' else 'make -j$(nproc)')
import base64 as _b64
_rule = ':msg, contains, "QObject: Cannot create children for a parent that is in a different thread" stop\n'
_b64str = _b64.b64encode(_rule.encode()).decode()
lines += [
    'sudo mv Jamulus /usr/bin/jamulus-jamfan',
    'sudo chmod +x /usr/bin/jamulus-jamfan',
    f'echo {_b64str} | base64 -d | sudo tee /etc/rsyslog.d/90-suppress-jamulus.conf > /dev/null && sudo systemctl reload rsyslog 2>/dev/null || true',
]
print('\n'.join(lines))
PYEOF
        if [[ "$gcc13" == "1" ]]; then
          echo "    caching ubuntu24/aarch64 binary for cross-deploy ..."
          scp -i ~/.ssh/id_ed25519 "$user@$host:/usr/bin/jamulus-jamfan" /tmp/jamfan-arm64-u24
          ARM64_U24_BINARY="/tmp/jamfan-arm64-u24"
        fi
        echo "    restarting services on $host ..."
        ssh -i ~/.ssh/id_ed25519 "$user@$host" "sudo systemctl start $host_services"
        echo "    done."
        continue
      fi
    fi
    active=$(ssh -i ~/.ssh/id_ed25519 "$user@$host" python3 <<PYCHECK 2>/dev/null || true
import socket, json
try:
    s = socket.socket(); s.connect(('127.0.0.1', ${rpcport})); s.settimeout(3)
    secret = open('${secret_file}').read().strip()
    def rpc(m, p={}):
        s.sendall((json.dumps({'id':1,'jsonrpc':'2.0','method':m,'params':p})+'\n').encode())
        return json.loads(s.recv(4096))
    rpc('jamulus/apiAuth', {'secret': secret})
    n = len(rpc('jamulusserver/getClients').get('result',{}).get('clients',[]))
    if n > 0: print(f'{n} client(s)')
    s.close()
except:
    pass
PYCHECK
)
    if [[ -n "$active" && "$FORCE" == "0" ]]; then
      echo "    binary updated, restart DEFERRED — $name has $active (use --force to override)"
    else
      ssh -i ~/.ssh/id_ed25519 "$user@$host" "sudo systemctl restart $service"
      echo "    done."
    fi

  else
    SSH_OPTS="-i ~/.ssh/id_ed25519 -o ConnectTimeout=30 -o BatchMode=yes"
    if ! scp $SSH_OPTS "$BINARY" "$user@$host:/tmp/jamulus-jamfan" 2>/dev/null; then
      [[ "$is_dormant" == "1" ]] && { echo "    SKIP_DORMANT $name — SCP failed (stopped mid-flight)"; continue; }
      scp -i ~/.ssh/id_ed25519 "$BINARY" "$user@$host:/tmp/jamulus-jamfan"
    fi
    if ! ssh $SSH_OPTS "$user@$host" \
      "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan && sudo chmod +x /usr/bin/jamulus-jamfan && $APPLY_RSYSLOG" 2>/dev/null; then
      [[ "$is_dormant" == "1" ]] && { echo "    SKIP_DORMANT $name — SSH failed (stopped mid-flight)"; continue; }
      ssh -i ~/.ssh/id_ed25519 "$user@$host" \
        "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan && sudo chmod +x /usr/bin/jamulus-jamfan && $APPLY_RSYSLOG"
    fi
    active=$(ssh -i ~/.ssh/id_ed25519 "$user@$host" python3 <<PYCHECK 2>/dev/null || true
import socket, json
try:
    s = socket.socket(); s.connect(('127.0.0.1', ${rpcport})); s.settimeout(3)
    secret = open('${secret_file}').read().strip()
    def rpc(m, p={}):
        s.sendall((json.dumps({'id':1,'jsonrpc':'2.0','method':m,'params':p})+'\n').encode())
        return json.loads(s.recv(4096))
    rpc('jamulus/apiAuth', {'secret': secret})
    n = len(rpc('jamulusserver/getClients').get('result',{}).get('clients',[]))
    if n > 0: print(f'{n} client(s)')
    s.close()
except:
    pass
PYCHECK
)
    if [[ -n "$active" && "$FORCE" == "0" ]]; then
      echo "    binary updated, restart DEFERRED — $name has $active (use --force to override)"
    else
      ssh -i ~/.ssh/id_ed25519 -o ConnectTimeout=30 "$user@$host" "sudo systemctl restart $service" 2>/dev/null \
        || { [[ "$is_dormant" == "1" ]] && echo "    SKIP_DORMANT $name — restart failed (stopped mid-flight)"; continue; }
      echo "    done."
    fi
  fi
done

# Keep jamfan22's fleet-server-ips.txt in sync with fleet.json
# 147.182.199.22 = lounge — trusted caller for /ip-allowed, not a game server
{ DORMANT_IPS="$DORMANT_IPS" python3 -c "
import json, os
fleet = json.load(open('$FLEET'))
dormant_ips = json.loads(os.environ.get('DORMANT_IPS', '{}'))
seen = set()
for s in fleet:
    iid = s.get('instance_id', '')
    ip = dormant_ips.get(iid, s['host']) if iid else s['host']
    port = s.get('port', 22224)
    rpcport = s.get('rpcport', 9999)
    key = (ip, port)
    if key in seen:
        continue
    seen.add(key)
    if 'dirhost' in s:
        dirhost, dirport = s['dirhost'], s['dirport']
    else:
        svc = s.get('service', 'jamulus-headless')
        if svc == 'jamulus-jazz':
            dirhost, dirport = 'jazz.jamulus.io', 22324
        elif svc == 'jamulus-ag2':
            dirhost, dirport = 'anygenre2.jamulus.io', 22224
        elif svc in ('jamulus-ag3', 'jamulus-ag3-sidecar'):
            dirhost, dirport = 'anygenre3.jamulus.io', 22624
        elif svc == 'jamulus-rock':
            dirhost, dirport = 'rock.jamulus.io', 22424
        else:
            dirhost, dirport = 'anygenre1.jamulus.io', 22124
    print(f'{ip}:{port}:{rpcport}:{dirhost}:{dirport}')
"
echo "147.182.199.22"
echo "24.199.107.192"
} | ssh -i ~/.ssh/id_ed25519 root@jamulus.live \
    'cat > /root/JamFan22/JamFan22/data/fleet-server-ips.txt'
echo "Fleet IPs synced to jamfan22."
