#!/usr/bin/env bash
# Usage: ./deploy.sh [<server-name-or-ip> | all]
# Deploys the built Jamulus binary to one or all fleet servers in fleet.json.
# Build first: qmake "CONFIG+=headless" Jamulus.pro && make -j$(nproc)

set -euo pipefail
cd "$(dirname "$0")"

FLEET="fleet.json"
BINARY="./Jamulus"

if [[ ! -f "$BINARY" ]]; then
  echo "No binary at $BINARY — build first:"
  echo "  qmake \"CONFIG+=headless\" Jamulus.pro && make -j\$(nproc)"
  exit 1
fi

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <server-name-or-ip | all>"
  echo
  echo "Known servers:"
  python3 -c "
import json
for s in json.load(open('$FLEET')):
    print(f\"  {s['name']:22s}  {s['host']:18s}  ({s['user']})\")
"
  exit 1
fi

TARGET="$1"

# Emit "host user" pairs for the target (or all)
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
    if s.get('arch') == 'aarch64':
        print(f\"SKIP {s['name']} (aarch64 — build natively on host)\", file=sys.stderr)
        continue
    service = s.get('service', 'jamulus-headless')
    print(s['host'], s['user'], service, s['name'])
" "$TARGET")

for pair in "${PAIRS[@]}"; do
  read -r host user service name <<< "$pair"
  echo "==> $name ($user@$host) [$service]"
  ssh-keyscan -H "$host" >> ~/.ssh/known_hosts 2>/dev/null
  scp -i ~/.ssh/id_ed25519 "$BINARY" "$user@$host:/tmp/jamulus-jamfan"
  ssh -i ~/.ssh/id_ed25519 "$user@$host" \
    "sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan \
     && sudo chmod +x /usr/bin/jamulus-jamfan \
     && sudo systemctl restart $service"
  echo "    done."
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
