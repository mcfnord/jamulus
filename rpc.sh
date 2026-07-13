#!/usr/bin/env bash
# Routes through the lounge (147.182.199.22), the only IP allowed to reach port 9999.
set -euo pipefail

FLEET_JSON="$(cd "$(dirname "$0")" && pwd)/fleet.json"
LOUNGE_IP="147.182.199.22"
SSH_KEY="$HOME/.ssh/id_ed25519"

if [[ $# -lt 2 ]]; then
    echo "Usage: rpc.sh <server-name|ip> <method> [json-params]" >&2
    echo "Example: rpc.sh 'Hot Texas!' jamulusserver/getClients" >&2
    echo "Example: rpc.sh 'Hot Texas!' jamulusserver/setRecordingBanner '{\"active\":true}'" >&2
    exit 1
fi

NAME="$1"
METHOD="$2"
PARAMS="${3:-{}}"

# RPC secret is kept out of source control: supply it via a local file.
RPC_SECRET_FILE="${JAMULUS_RPC_SECRET_FILE:-$HOME/.jamulus-rpc-secret}"
[[ -s "$RPC_SECRET_FILE" ]] || { echo "No RPC secret at $RPC_SECRET_FILE (set JAMULUS_RPC_SECRET_FILE)" >&2; exit 1; }
RPC_SECRET="$(tr -d '\n' < "$RPC_SECRET_FILE")"

read -r FLEET_HOST FLEET_RPCPORT < <(python3 -c "
import json, sys
fleet = json.load(open(sys.argv[1]))
needle = sys.argv[2].lower()
for s in fleet:
    if s['name'].lower() == needle or s['host'] == needle:
        print(s['host'], s.get('rpcport', 9999))
        sys.exit(0)
print(f'Server not found: {sys.argv[2]}', file=sys.stderr)
sys.exit(1)
" "$FLEET_JSON" "$NAME")

# Run on the lounge; it connects to the fleet server's public IP:rpcport
ssh -i "$SSH_KEY" "root@$LOUNGE_IP" python3 - "$FLEET_HOST" "$FLEET_RPCPORT" "$METHOD" "$PARAMS" "$RPC_SECRET" << 'PYEOF'
import socket, json, sys
host, rpcport, method, params, secret = sys.argv[1], int(sys.argv[2]), sys.argv[3], json.loads(sys.argv[4]), sys.argv[5]
s = socket.socket()
s.connect((host, rpcport))
s.settimeout(5)
def rpc(m, p={}):
    s.sendall((json.dumps({'id':1,'jsonrpc':'2.0','method':m,'params':p})+'\n').encode())
    return json.loads(s.recv(4096))
rpc('jamulus/apiAuth', {'secret': secret})
print(json.dumps(rpc(method, params), indent=2))

s.close()
PYEOF
