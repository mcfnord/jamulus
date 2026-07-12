#!/usr/bin/env bash
# Sync fleet server IPs to jamfan22's fleet-server-ips.txt.
# Run this whenever fleet.json changes without a corresponding deploy.
set -euo pipefail
cd "$(dirname "$0")"

# Fetch current IPs for dormant instances from jamulus.live cache.
# Falls back to fleet.json host field if cache is unavailable.
DORMANT_IPS=$(ssh -i ~/.ssh/id_ed25519 root@jamulus.live 'cat /root/dormant-ip-cache.json 2>/dev/null || echo "{}"' 2>/dev/null || echo '{}')

{ DORMANT_IPS="$DORMANT_IPS" python3 - <<'PYEOF'
import json, os, sys
fleet = json.load(open('fleet.json'))
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
PYEOF
echo "147.182.199.22"
echo "24.199.107.192"
for p in 22121 22122 22123 22124 22125 22126 22127; do echo "24.199.107.192:$p"; done
} | ssh -i ~/.ssh/id_ed25519 root@jamulus.live \
    'cat > /root/JamFan22/JamFan22/data/fleet-server-ips.txt'

# Regenerate fleet-rpc-ports.txt (non-9999 RPC port mappings, format: ip:gameport=rpcport)
{ DORMANT_IPS="$DORMANT_IPS" python3 - <<'PYEOF'
import json, os
fleet = json.load(open('fleet.json'))
dormant_ips = json.loads(os.environ.get('DORMANT_IPS', '{}'))
for s in fleet:
    rpcport = s.get('rpcport', 9999)
    if rpcport == 9999:
        continue
    iid = s.get('instance_id', '')
    ip = dormant_ips.get(iid, s['host']) if iid else s['host']
    port = s.get('port', 22224)
    print(f'{ip}:{port}={rpcport}')
PYEOF
} | ssh -i ~/.ssh/id_ed25519 root@jamulus.live \
    'cat > /root/JamFan22/JamFan22/data/fleet-rpc-ports.txt'

echo "Synced $(python3 -c "import json; print(len(json.load(open('fleet.json'))))" ) servers to jamfan22."
