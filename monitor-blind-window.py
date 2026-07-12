#!/usr/bin/env python3
"""
Monitor getClients RPC across fleet servers to detect "blind window" events:
clients that appear with incomplete metadata (empty name, default country "-")
and later get populated with real data in the same session.

Each server is polled in its own thread, so poll_ms applies per-server independently.

Run ON the lounge (147.182.199.22), which has direct TCP access to fleet RPC ports.

Usage:
  python3 monitor-blind-window.py [poll_ms]   # default 100ms
"""

import argparse
import socket
import json
import time
import sys
import threading
from datetime import datetime, timezone

_ap = argparse.ArgumentParser()
_ap.add_argument('poll_ms', nargs='?', default=100.0, type=float)
_ap.add_argument('--fleet', default=None, metavar='PATH',
                 help='fleet.json path; scp to this host before running')
_ap = _ap.parse_args()
POLL_INTERVAL = _ap.poll_ms / 1000

SECRET = "REDACTED-SECRET"

if _ap.fleet:
    with open(_ap.fleet) as _jf:
        _fdata = json.load(_jf)
    FLEET = [
        {"name": e["name"], "host": e["host"], "port": e.get("rpcport", 9999)}
        for e in _fdata
        if not e.get("dormant")
        and not e.get("skip")
        and not e.get("docker")
        and not e.get("secret_file")
    ]
else:
    FLEET = [
        {"name": "Hot Texas!",    "host": "50.116.25.151",  "port": 9999},
        {"name": "Studio D",      "host": "24.199.127.71",  "port": 9999},
        {"name": "Sindone",       "host": "92.4.218.204",   "port": 9999},
        {"name": "Valdo",         "host": "92.4.218.204",   "port": 9998},
        {"name": "Rising",        "host": "132.226.27.144", "port": 9999},
        {"name": "Freiheit",      "host": "130.61.155.141", "port": 9999},
        {"name": "Jazzstübchen",  "host": "130.61.155.141", "port": 9998},
        {"name": "Cutlove",       "host": "18.170.218.139", "port": 9999},
        {"name": "Ronnie Scott's","host": "18.170.218.139", "port": 9998},
        {"name": "Paradiso",      "host": "178.128.253.5",  "port": 9999},
        {"name": "Melkweg",       "host": "178.128.253.5",  "port": 9998},
        {"name": "Bimhuis",       "host": "178.128.253.5",  "port": 9997},
    ]

print_lock = threading.Lock()


def ts():
    return datetime.now(timezone.utc).strftime("%H:%M:%S.%f")[:-3] + "Z"


def log(msg):
    with print_lock:
        print(msg, flush=True)


def is_incomplete(c):
    """True if this client lacks profile info: empty name OR country still at default '-'."""
    name = c.get("name", "").strip()
    country = c.get("countryName", "-")
    return not name or country == "-"


class ServerMonitor:
    def __init__(self, entry):
        self.label = entry["name"]
        self.host = entry["host"]
        self.port = entry["port"]
        self.sock = None
        self.req_id = 1
        self.known = {}       # channel_id -> state dict
        self.join_count = 0
        self.blind_events = []
        self._lock = threading.Lock()

    def connect(self):
        try:
            s = socket.socket()
            s.settimeout(5)
            s.connect((self.host, self.port))
            s.sendall((json.dumps({
                "id": 0, "jsonrpc": "2.0",
                "method": "jamulus/apiAuth",
                "params": {"secret": SECRET}
            }) + "\n").encode())
            s.recv(4096)
            self.sock = s
            log(f"[{ts()}] [{self.label}] connected")
        except Exception as e:
            log(f"[{ts()}] [{self.label}] connect failed: {e}")
            self.sock = None

    def _recv_line(self):
        data = b""
        while b"\n" not in data:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            data += chunk
        return data.split(b"\n")[0]

    def poll(self):
        if self.sock is None:
            self.connect()
            return

        try:
            self.sock.sendall((json.dumps({
                "id": self.req_id, "jsonrpc": "2.0",
                "method": "jamulusserver/getClients",
                "params": {}
            }) + "\n").encode())
            self.req_id += 1
            resp = json.loads(self._recv_line())
            clients = resp.get("result", {}).get("clients", [])
            now = time.monotonic()

            current_ids = set()
            for c in clients:
                cid = c.get("id")
                addr = c.get("address", "?")
                current_ids.add(cid)

                if cid not in self.known:
                    self.join_count += 1
                    incomplete = is_incomplete(c)
                    self.known[cid] = {
                        "first_seen": now,
                        "first_complete": None if incomplete else now,
                        "was_incomplete": incomplete,
                        "addr": addr,
                    }
                    if incomplete:
                        log(
                            f"[{ts()}] [{self.label}] BLIND JOIN  "
                            f"ch={cid} addr={addr} "
                            f"name={repr(c.get('name',''))} "
                            f"country={c.get('countryName','-')} "
                            f"instr={c.get('instrumentCode','?')}"
                        )
                    else:
                        log(
                            f"[{ts()}] [{self.label}] clean join  "
                            f"ch={cid} addr={addr} "
                            f"name={repr(c.get('name',''))} "
                            f"country={c.get('countryName','-')}"
                        )
                else:
                    entry = self.known[cid]
                    if entry["was_incomplete"] and entry["first_complete"] is None:
                        if not is_incomplete(c):
                            delay = now - entry["first_seen"]
                            entry["first_complete"] = now
                            evt = {
                                "server": self.label,
                                "addr": entry["addr"],
                                "blind_s": round(delay, 3),
                                "final_name": c.get("name", ""),
                                "final_country": c.get("countryName", "-"),
                            }
                            self.blind_events.append(evt)
                            log(
                                f"[{ts()}] [{self.label}] POPULATED   "
                                f"ch={cid} after {delay:.3f}s → "
                                f"name={repr(c.get('name',''))} "
                                f"country={c.get('countryName','-')}"
                            )

            for cid in list(self.known.keys()):
                if cid not in current_ids:
                    entry = self.known.pop(cid)
                    if entry["was_incomplete"] and entry["first_complete"] is None:
                        elapsed = now - entry["first_seen"]
                        log(
                            f"[{ts()}] [{self.label}] LEFT BLIND  "
                            f"ch={cid} addr={entry['addr']} "
                            f"after {elapsed:.3f}s (never populated)"
                        )

        except Exception as e:
            log(f"[{ts()}] [{self.label}] poll error: {e}")
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def run_loop(self, stop_event):
        self.connect()
        while not stop_event.is_set():
            t0 = time.monotonic()
            self.poll()
            elapsed = time.monotonic() - t0
            sleep = max(0, POLL_INTERVAL - elapsed)
            time.sleep(sleep)


def main():
    stop_event = threading.Event()
    monitors = [ServerMonitor(f) for f in FLEET]
    threads = []
    for m in monitors:
        t = threading.Thread(target=m.run_loop, args=(stop_event,), daemon=True)
        t.start()
        threads.append(t)

    log(f"\nPolling {len(monitors)} servers at {POLL_INTERVAL*1000:.0f}ms per server (threaded). Ctrl-C to stop.\n")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        stop_event.set()
        for t in threads:
            t.join(timeout=2)

        print("\n\n=== SUMMARY ===")
        total_joins = sum(m.join_count for m in monitors)
        total_blind = sum(len(m.blind_events) for m in monitors)
        print(f"Total joins observed : {total_joins}")
        print(f"Blind-window events  : {total_blind}")
        if total_joins:
            print(f"Blind rate           : {100*total_blind/total_joins:.1f}%")
        print()
        for m in monitors:
            if m.blind_events:
                delays = [e["blind_s"] for e in m.blind_events]
                print(f"  {m.label}: {len(m.blind_events)} blind events")
                print(f"    avg {sum(delays)/len(delays):.3f}s  "
                      f"min {min(delays):.3f}s  max {max(delays):.3f}s")
                for e in m.blind_events:
                    print(f"    {e['addr']}  blind={e['blind_s']}s  "
                          f"→ {repr(e['final_name'])} / {e['final_country']}")
        print()
        print("Note: 'LEFT BLIND' events appear in the log but are excluded from summary stats.")


if __name__ == "__main__":
    main()
