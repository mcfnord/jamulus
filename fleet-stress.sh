#!/usr/bin/env bash
# fleet-stress.sh — run gjstress on fleet servers to simulate multi-IP load on a target
#
# Usage:
#   ./fleet-stress.sh <target-name>          # e.g. "Freiheit"
#   ./fleet-stress.sh <target-name> --noise  # amplitude-modulated white noise
#   ./fleet-stress.sh <target-name> --bots 2 # 2 bots per fleet server (default: 1)
#
# Two built-in test modes (set TEST env var):
#   TEST=regression  — join simultaneously, watch for cache-miss storm in first 30s
#   TEST=boundary    — hold 11 minutes, stress the 600s TTL expiry window
#
# The target server's --directory arg is auto-detected from fleet.json (OCI/AWS need it).
# gjstress binaries are uploaded to each fleet server on first run and reused if present.
#
# Output: per-bot rx% lines prefixed with [ServerName], plus a timestamped server journal
# log captured in parallel. Summary printed at end: bot pass/fail + journal event count.

set -euo pipefail
cd "$(dirname "$0")"

FLEET="fleet.json"
X86_BINARY="/root/gojam/gjstress"
ARM64_BINARY_LOUNGE="/usr/local/bin/gjstress"  # pulled from lounge
TARGET_NAME="${1:-Freiheit}"
shift || true

# Parse optional args
BOTS=1
AUDIO_FLAG="--tone"
DURATION="3m"
for arg in "$@"; do
    case "$arg" in
        --bots) ;;
        --noise) AUDIO_FLAG="--noise" ;;
        --tone)  AUDIO_FLAG="--tone"  ;;
        --duration) ;;
        [0-9]*) BOTS="$arg" ;;
        *m|*s)  DURATION="$arg" ;;
    esac
done

case "${TEST:-}" in
    boundary)   DURATION="11m" ;;
    regression) DURATION="3m"  ;;
esac

# Look up target in fleet.json
TARGET_INFO=$(python3 -c "
import json, sys
fleet = json.load(open('$FLEET'))
t = next((s for s in fleet if s['name'].lower() == '${TARGET_NAME}'.lower()), None)
if not t:
    print('ERROR: no server named ${TARGET_NAME}', file=sys.stderr); sys.exit(1)
port    = t.get('port', 22224)
user    = t['user']
service = t.get('service', 'jamulus-headless')
dirhost = t.get('dirhost', '')
dirport = t.get('dirport', 0)
needs_punch = 'oracle' in t.get('os','') or t.get('gcc13_fix', False)
if needs_punch and not dirhost:
    dirhost = 'anygenre1.jamulus.io'; dirport = 22124
dir_arg = f'--directory {dirhost}:{dirport}' if dirhost else ''
import json as j
print(j.dumps({'addr': f\"{t['host']}:{port}\", 'user': user, 'service': service, 'dir_arg': dir_arg}))
")
TARGET_ADDR=$(echo "$TARGET_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['addr'])")
TARGET_USER=$(echo "$TARGET_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['user'])")
TARGET_SERVICE=$(echo "$TARGET_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['service'])")
DIR_ARG=$(echo "$TARGET_INFO" | python3 -c "import json,sys; print(json.load(sys.stdin)['dir_arg'])")
TARGET_HOST=$(echo "$TARGET_ADDR" | cut -d: -f1)

# Log files for this run
RUN_ID=$(date +%Y%m%d-%H%M%S)
LOG_DIR="/tmp/fleet-stress-${RUN_ID}"
mkdir -p "$LOG_DIR"
SERVER_LOG="$LOG_DIR/server-journal.log"
BOT_LOG="$LOG_DIR/bots.log"

echo "==> fleet-stress: target=$TARGET_NAME ($TARGET_ADDR) bots=$BOTS duration=$DURATION $AUDIO_FLAG ${DIR_ARG:-(no hole-punch)} test=${TEST:-manual}"
echo "==> Logs: $LOG_DIR"
echo

# Fetch aarch64 binary from lounge
ARM64_BINARY="/tmp/gjstress-arm64"
if [[ ! -f "$ARM64_BINARY" ]]; then
    echo "==> Fetching aarch64 gjstress from lounge..."
    scp -i ~/.ssh/id_ed25519 root@147.182.199.22:"$ARM64_BINARY_LOUNGE" "$ARM64_BINARY"
fi

# Build list of fleet servers to use as bots (exclude target host, skip=true servers)
BOTS_JSON=$(python3 -c "
import json, sys
fleet = json.load(open('$FLEET'))
seen_hosts = set()
bots = []
for s in fleet:
    if s.get('skip'): continue
    if s['host'] == '$TARGET_HOST': continue
    if s['host'] in seen_hosts: continue
    seen_hosts.add(s['host'])
    arch = s.get('arch', 'x86_64')
    bots.append({'host': s['host'], 'user': s['user'], 'arch': arch, 'name': s['name']})
print(json.dumps(bots))
")
BOT_COUNT=$(echo "$BOTS_JSON" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")

# Upload binaries to all fleet servers first, before synchronized start
echo "==> Preparing $BOT_COUNT fleet servers..."
while IFS= read -r bot; do
    host=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['host'])")
    user=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['user'])")
    arch=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['arch'])")
    name=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['name'])")
    LOCAL_BIN=$( [[ "$arch" == "aarch64" ]] && echo "$ARM64_BINARY" || echo "$X86_BINARY" )
    REMOTE_SUM=$(ssh -i ~/.ssh/id_ed25519 "$user@$host" "sha256sum /tmp/gjstress-stress 2>/dev/null | awk '{print \$1}'" || echo "")
    LOCAL_SUM=$(sha256sum "$LOCAL_BIN" | awk '{print $1}')
    if [[ "$REMOTE_SUM" != "$LOCAL_SUM" ]]; then
        echo "  uploading to $name..."
        scp -i ~/.ssh/id_ed25519 "$LOCAL_BIN" "$user@$host:/tmp/gjstress-stress"
        ssh -i ~/.ssh/id_ed25519 "$user@$host" "chmod +x /tmp/gjstress-stress"
    else
        echo "  $name: binary current"
    fi
done < <(echo "$BOTS_JSON" | python3 -c "import json,sys; [print(json.dumps(b)) for b in json.load(sys.stdin)]")

# Start server-side journal capture in background
# Captures all CentralDefense lines with timestamps; writes to SERVER_LOG
TEST_START=$(date -u '+%Y-%m-%d %H:%M:%S')
echo "==> Starting server journal capture on $TARGET_NAME..."
ssh -i ~/.ssh/id_ed25519 "$TARGET_USER@$TARGET_HOST" \
    "journalctl -u $TARGET_SERVICE -f --output=short-iso 2>/dev/null | grep --line-buffered -E 'centraldefense|cache-miss|cached-allow|cached-block|lookup|precheck'" \
    > "$SERVER_LOG" 2>/dev/null &
JOURNAL_PID=$!

# Allow journal tail to connect before bots start
sleep 2

# Synchronized start: bots sleep until START_EPOCH
START_EPOCH=$(( $(date +%s) + 5 ))
echo "==> Launching $BOT_COUNT bots simultaneously at $(date -d "@$START_EPOCH" '+%H:%M:%S' 2>/dev/null || date -r "$START_EPOCH" '+%H:%M:%S')..."

PIDS=()
BOT_NAMES=()

while IFS= read -r bot; do
    host=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['host'])")
    user=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['user'])")
    name=$(echo "$bot" | python3 -c "import json,sys; b=json.load(sys.stdin); print(b['name'])")
    CMD="sleep \$(( $START_EPOCH - \$(date +%s) )) && /tmp/gjstress-stress --target $TARGET_ADDR --bots $BOTS --duration $DURATION $AUDIO_FLAG ${DIR_ARG} 2>&1"
    ssh -i ~/.ssh/id_ed25519 "$user@$host" "$CMD" \
        | while IFS= read -r line; do
            TS=$(date '+%H:%M:%S')
            echo "[$TS][$name] $line" | tee -a "$BOT_LOG"
          done &
    PIDS+=($!)
    BOT_NAMES+=("$name")
done < <(echo "$BOTS_JSON" | python3 -c "import json,sys; [print(json.dumps(b)) for b in json.load(sys.stdin)]")

echo "==> Bots running. Ctrl-C to abort."
echo

# Wait for all bots
FAILED=0
for i in "${!PIDS[@]}"; do
    wait "${PIDS[$i]}" || { echo "  FAILED: ${BOT_NAMES[$i]}"; FAILED=$(( FAILED + 1 )); }
done

# Stop journal capture
kill "$JOURNAL_PID" 2>/dev/null || true
wait "$JOURNAL_PID" 2>/dev/null || true

echo
echo "========================================================"
echo " RESULTS — $TARGET_NAME — $(date -u '+%Y-%m-%d %H:%M UTC')"
echo "========================================================"
echo

# Bot summary: extract final rx% lines (last window per bot)
echo "--- Bot rx% (last 10s window per source) ---"
grep -E '\bwindow\b|\brx\b|avg=' "$BOT_LOG" 2>/dev/null | grep -E '\[.*\]' | \
    awk -F']' '{key=$2; line=$0} END{}' || true
# Show all DEGRADED/warn lines
DEGRADED=$(grep -cE 'DEGRADED|warn' "$BOT_LOG" 2>/dev/null || echo 0)
echo "  Degraded/warn windows across all bots: $DEGRADED"
echo "  Full bot log: $BOT_LOG"
echo

# Server journal summary
echo "--- Server journal (CentralDefense events) ---"
TOTAL_LINES=$(wc -l < "$SERVER_LOG" 2>/dev/null || echo 0)
CACHE_MISS=$(grep -c 'cache-miss' "$SERVER_LOG" 2>/dev/null || echo 0)
ENQUEUE=$(grep -c 'enqueue' "$SERVER_LOG" 2>/dev/null || echo 0)
LOOKUP_FINISH=$(grep -c 'lookup: finish' "$SERVER_LOG" 2>/dev/null || echo 0)
echo "  Total CentralDefense journal lines: $TOTAL_LINES"
echo "  cache-miss lines:   $CACHE_MISS  (should be 0 with fix — these are qCDebug now)"
echo "  lookup: enqueue:    $ENQUEUE     (one per unique IP joining — expected)"
echo "  lookup: finish:     $LOOKUP_FINISH"
echo "  Full server log: $SERVER_LOG"
echo

# Verdict
echo "--- Verdict ---"
if [[ "$CACHE_MISS" -eq 0 && "$DEGRADED" -eq 0 ]]; then
    echo "  PASS: no cache-miss storm, no rx% degradation."
elif [[ "$CACHE_MISS" -gt 100 ]]; then
    echo "  FAIL: cache-miss storm detected ($CACHE_MISS lines) — fix may not be active."
elif [[ "$DEGRADED" -gt 0 ]]; then
    echo "  WARN: rx% degraded in $DEGRADED window(s) — investigate bot log."
else
    echo "  WARN: $CACHE_MISS cache-miss lines (low, may be debug noise — check log)."
fi
echo "========================================================"
