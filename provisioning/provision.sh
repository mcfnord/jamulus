#!/usr/bin/env bash
# Usage: ./provisioning/provision.sh <host> [user]
# Provisions a fresh Ubuntu 24.04 server as a Jamulus fleet node.
# Run from the repo root after a successful build (./Jamulus binary present).
# After running, manually write the service file and update fleet.json.

set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:?Usage: $0 <host> [user]}"
USER="${2:-ubuntu}"
BINARY="./Jamulus"

if [[ ! -f "$BINARY" ]]; then
  echo "No binary at $BINARY — build first:"
  echo "  qmake \"CONFIG+=headless\" Jamulus.pro && make -j\$(nproc)"
  exit 1
fi

echo "==> Adding host key"
ssh-keyscan -H "$HOST" >> ~/.ssh/known_hosts 2>/dev/null

echo "==> Copying binary"
scp -i ~/.ssh/id_ed25519 "$BINARY" "$USER@$HOST:/tmp/jamulus-jamfan"

# Fleet RPC secret is kept out of source control. Provide it via a local file
# (default ~/.jamulus-rpc-secret) or the JAMULUS_RPC_SECRET_FILE env var.
RPC_SECRET_FILE="${JAMULUS_RPC_SECRET_FILE:-$HOME/.jamulus-rpc-secret}"
if [[ ! -s "$RPC_SECRET_FILE" ]]; then
  echo "No RPC secret found at $RPC_SECRET_FILE."
  echo "  Put the fleet RPC secret there (or set JAMULUS_RPC_SECRET_FILE), then re-run."
  exit 1
fi

echo "==> Installing binary and dependencies"
ssh -i ~/.ssh/id_ed25519 "$USER@$HOST" '
  sudo mv /tmp/jamulus-jamfan /usr/bin/jamulus-jamfan
  sudo chmod +x /usr/bin/jamulus-jamfan
  sudo apt-get install -y libqt5core5a libqt5network5 libqt5xml5 libjack-jackd2-0 libqt5widgets5 libqt5multimedia5 libqt5websockets5
  printf ":msg, contains, \"QObject: Cannot create children for a parent that is in a different thread\" stop\n" | sudo tee /etc/rsyslog.d/90-suppress-jamulus.conf > /dev/null
  sudo systemctl reload rsyslog 2>/dev/null || true
  # Cap syslog size so it can never fill a small instance disk between the stock
  # weekly rotations (default logrotate has no size check; a busy host can grow
  # syslog into the multi-GB range in days, well before the next weekly rotation).
  # Patch the existing /etc/logrotate.d/rsyslog stanza in place (rather than add a
  # competing file for the same path, which logrotate flags as a duplicate entry).
  # size 200M rides the existing daily cron.daily/logrotate run -- no new cron job needed.
  sudo sed -i "s/^\tweekly$/\tweekly\n\tsize 200M/" /etc/logrotate.d/rsyslog
  # /var/log is 775 root:syslog by design (lets rsyslog log as non-root) -- but logrotate
  # refuses to rotate anything in a group-writable dir unless told who to act as.
  # Without this, the size cap above silently never fires.
  sudo sed -i "s/^{$/{\n\tsu root syslog/" /etc/logrotate.d/rsyslog
  sudo adduser --system --quiet --home /nonexistent --no-create-home jamulus 2>/dev/null || true
  sudo mkdir -p /etc/jamulus && sudo touch /etc/jamulus/welcome.html
'

# Write the RPC secret over stdin to avoid embedding it or hitting shell-quoting hazards.
tr -d "\n" < "$RPC_SECRET_FILE" | ssh -i ~/.ssh/id_ed25519 "$USER@$HOST" 'sudo tee /secret.txt > /dev/null'

echo "==> Copying service file"
scp -i ~/.ssh/id_ed25519 provisioning/jamulus-headless.service "$USER@$HOST:/tmp/jamulus-headless.service"
ssh -i ~/.ssh/id_ed25519 "$USER@$HOST" '
  sudo mv /tmp/jamulus-headless.service /etc/systemd/system/jamulus-headless.service
  sudo systemctl daemon-reload
  sudo systemctl enable --now jamulus-headless
'

echo "==> Done. Edit --serverinfo in the service file on the server, then:"
echo "    ssh $USER@$HOST 'sudo systemctl daemon-reload && sudo systemctl restart jamulus-headless'"
echo "    Then add the server to fleet.json."
