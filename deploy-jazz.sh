#!/usr/bin/env bash
# Deploys Jazz sidecar servers to fleet instances (all except Studio D).
# Uses the existing /usr/bin/jamulus-jamfan binary — no build needed.
# Jazz servers run on port 22225, capacity 6, genre Jazz.

set -euo pipefail
cd "$(dirname "$0")"

python3 << 'PYEOF'
import subprocess, os, tempfile, json

WELCOME = {
    'English':    '<br>See https://jamulus.live for a live view of Jamulus public servers.',
    'German':     '<br>Unter https://jamulus.live findest du eine Live-Übersicht öffentlicher Jamulus-Server.',
    'Italian':    '<br>See https://jamulus.live for a live view of Jamulus public servers.',
    'French':     '<br>Voir https://jamulus.live pour un aperçu en direct des serveurs Jamulus publics.',
    'Dutch':      '<br>Zie https://jamulus.live voor een liveoverzicht van openbare Jamulus-servers.',
    'Spanish':    '<br>Ver https://jamulus.live para una vista en vivo de los servidores públicos de Jamulus.',
    'Portuguese': '<br>Veja https://jamulus.live para uma visão ao vivo dos servidores Jamulus públicos.',
}

with open('fleet.json') as jf:
    fleet = json.load(jf)

SERVERS = [
    (e['host'], e['user'], e['name'], e['cc'], e.get('maxclients', 6),
     WELCOME.get(e['lang'], WELCOME['English']))
    for e in fleet
    if e.get('service') == 'jamulus-jazz'
    and not e.get('dormant')
    and not e.get('skip')
    and not e.get('secret_file')
    and 'cc' in e
]

SERVICE_TMPL = """\
[Unit]
Description=Jamulus Jazz sidecar server
After=network.target
StartLimitIntervalSec=0

[Service]
User=jamulus
Group=nogroup
NoNewPrivileges=true
ProtectSystem=true
Nice=-20
IOSchedulingClass=realtime
IOSchedulingPriority=0
ExecStart=/bin/sh -c "exec /usr/bin/jamulus-jamfan --nogui --server --port 22225 --directoryserver jazz.jamulus.io:22324 --serverinfo \\"{name};;{cc}\\" -u {capacity} -w \\"{welcome}\\""
Restart=on-failure
RestartSec=30
SyslogIdentifier=jamulus-jazz

[Install]
WantedBy=multi-user.target
"""

key = os.path.expanduser('~/.ssh/id_ed25519')

for host, user, name, cc, capacity, welcome in SERVERS:
    print(f'==> {name} ({user}@{host})')
    content = SERVICE_TMPL.format(name=name, cc=cc, capacity=capacity, welcome=welcome)

    with tempfile.NamedTemporaryFile(mode='w', suffix='.service', delete=False) as f:
        f.write(content)
        tmpfile = f.name

    try:
        subprocess.run(['scp', '-i', key, tmpfile, f'{user}@{host}:/tmp/jamulus-jazz.service'], check=True)
        subprocess.run(['ssh', '-i', key, f'{user}@{host}',
            'sudo mv /tmp/jamulus-jazz.service /etc/systemd/system/jamulus-jazz.service'
            ' && sudo systemctl daemon-reload'
            ' && sudo systemctl enable jamulus-jazz'
            ' && sudo systemctl restart jamulus-jazz'], check=True)
        print('    done.')
    finally:
        os.unlink(tmpfile)

PYEOF
