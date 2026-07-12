#!/usr/bin/env python3
"""
Dormant instance demand monitor — long-running daemon.

Reads census.csv every 30 minutes, scores geographic demand near each
configured dormant AWS instance, and starts/stops them via boto3.
Geolocation results are cached in RAM; new lookups rate-limited to 1 req/sec.

Run on jamulus.live as a service (census.csv is local there).
Requires: boto3, requests  (both available via apt on jamulus.live)
AWS credentials: AWS_ACCESS_KEY_ID + AWS_SECRET_ACCESS_KEY env vars.
"""

import json
import math
import time
import logging
import os
import sys
from collections import defaultdict
from datetime import datetime, timezone

import boto3
import requests

# ---------------------------------------------------------------------------
# Dormant instance config — loaded from dormant-instances.json
# Edit that file to add/remove instances; no restart needed for threshold changes
# (those are overridden live by dormant-thresholds.json).
# ---------------------------------------------------------------------------

INSTANCES_FILE = '/root/dormant-instances.json'

with open(INSTANCES_FILE) as _f:
    INSTANCES = json.load(_f)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

CENSUS_CSV           = '/root/JamFan22/JamFan22/data/census.csv'
FLEET_SERVER_IPS_TXT = '/root/JamFan22/JamFan22/data/fleet-server-ips.txt'
FLEET_RPC_PORTS_TXT  = '/root/JamFan22/JamFan22/data/fleet-rpc-ports.txt'
DORMANT_IP_CACHE     = '/root/dormant-ip-cache.json'

# instance_id → current public IP; populated at startup, kept live on start events
_current_ips: dict[str, str | None] = {}
EPOCH            = datetime(2023, 1, 1, tzinfo=timezone.utc)
CHECK_INTERVAL   = 20 * 60   # seconds between demand checks
LOOKBACK_MINUTES = 60        # census lookback window
GEO_RATE_SEC     = 1.0       # min seconds between geolocation requests

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [DORMANT] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S',
    stream=sys.stdout,
)
log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Geolocation (RAM cache, 1 req/sec)
# ---------------------------------------------------------------------------

_geo_cache: dict[str, tuple[float, float] | None] = {}
_geo_last_request: float = 0.0


def geolocate(ip: str) -> tuple[float, float] | None:
    global _geo_last_request
    if ip in _geo_cache:
        return _geo_cache[ip]
    wait = GEO_RATE_SEC - (time.monotonic() - _geo_last_request)
    if wait > 0:
        time.sleep(wait)
    result = None
    try:
        r = requests.get(
            f'http://ip-api.com/json/{ip}',
            params={'fields': 'status,lat,lon'},
            timeout=5,
        )
        _geo_last_request = time.monotonic()
        d = r.json()
        if d.get('status') == 'success':
            result = (float(d['lat']), float(d['lon']))
    except Exception as e:
        _geo_last_request = time.monotonic()
        log.warning(f"geolocate {ip}: {e}")
    _geo_cache[ip] = result
    return result


# ---------------------------------------------------------------------------
# Haversine
# ---------------------------------------------------------------------------

def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    R = 6371.0
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = (math.sin(dlat / 2) ** 2
         + math.cos(math.radians(lat1)) * math.cos(math.radians(lat2))
         * math.sin(dlon / 2) ** 2)
    return R * 2 * math.asin(math.sqrt(a))


# ---------------------------------------------------------------------------
# Census query
# ---------------------------------------------------------------------------

def active_guids_by_ip() -> dict[str, set[str]]:
    """Return {server_ip: {guid, ...}} for all rows in the last LOOKBACK_MINUTES."""
    now_min = int((datetime.now(timezone.utc) - EPOCH).total_seconds() / 60)
    cutoff  = now_min - LOOKBACK_MINUTES
    result: dict[str, set[str]] = defaultdict(set)
    try:
        with open(CENSUS_CSV) as f:
            for line in f:
                parts = line.rstrip('\n').split(',')
                if len(parts) < 3:
                    continue
                try:
                    ts = int(parts[0])
                except ValueError:
                    continue
                if ts < cutoff:
                    continue
                ip = parts[2].split(':')[0]
                result[ip].add(parts[1])
    except OSError as e:
        log.error(f"Reading census.csv: {e}")
    return result


# ---------------------------------------------------------------------------
# Demand score
# ---------------------------------------------------------------------------

def demand_score(guids_by_ip: dict[str, set[str]], inst_lat: float, inst_lon: float) -> float:
    score = 0.0
    for ip, guids in guids_by_ip.items():
        loc = geolocate(ip)
        if loc is None:
            continue
        dist_km = max(haversine_km(loc[0], loc[1], inst_lat, inst_lon), 1.0)
        score += len(guids) / dist_km
    return score


# ---------------------------------------------------------------------------
# AWS helpers
# ---------------------------------------------------------------------------

def load_thresholds() -> dict[str, float]:
    try:
        with open(INSTANCES_FILE) as f:
            return {inst['name']: inst['threshold'] for inst in json.load(f)}
    except (OSError, json.JSONDecodeError):
        return {}


def instance_state(ec2, instance_id: str) -> str:
    resp = ec2.describe_instances(InstanceIds=[instance_id])
    return resp['Reservations'][0]['Instances'][0]['State']['Name']


# ---------------------------------------------------------------------------
# Per-instance check
# ---------------------------------------------------------------------------

def check_instance(inst: dict, ec2_clients: dict, guids_by_ip: dict, streak: int, threshold: float) -> int:
    score = demand_score(guids_by_ip, inst['lat'], inst['lon'])
    ec2   = ec2_clients[inst['name']]
    state = instance_state(ec2, inst['instance_id'])

    log.info(
        f"{inst['name']}: score={score:.4f}  instance={state}"
        f"  threshold={threshold}"
    )

    if state in ('pending', 'stopping', 'shutting-down'):
        log.info(f"{inst['name']}: transitional state ({state}), skipping.")
        return streak

    if score >= threshold and state == 'stopped':
        log.info(f"{inst['name']}: starting (score {score:.4f} >= {threshold}).")
        ec2.start_instances(InstanceIds=[inst['instance_id']])
        log.info(f"{inst['name']}: polling for new public IP...")
        new_ip = _poll_for_ip(inst, ec2)
        if new_ip:
            old_ip = _current_ips.get(inst['instance_id'])
            _current_ips[inst['instance_id']] = new_ip
            if old_ip and old_ip != new_ip:
                log.info(f"{inst['name']}: IP changed {old_ip} → {new_ip}, updating fleet files")
                _patch_file_ips(FLEET_SERVER_IPS_TXT, old_ip, new_ip)
                _patch_file_ips(FLEET_RPC_PORTS_TXT, old_ip, new_ip)
            _write_ip_cache()
        return 0

    if score < threshold and state == 'running':
        streak += 1
        log.info(f"{inst['name']}: below threshold, streak={streak}/{inst['stop_streak']}.")
        if streak >= inst['stop_streak']:
            log.info(f"{inst['name']}: stopping after {inst['stop_streak']} low checks.")
            ec2.stop_instances(InstanceIds=[inst['instance_id']])
            return 0
    else:
        streak = 0

    return streak


# ---------------------------------------------------------------------------
# Dynamic IP tracking — patches fleet files when dormant instances get new IPs
# ---------------------------------------------------------------------------

def _patch_file_ips(path: str, old_ip: str, new_ip: str) -> None:
    """Replace line-leading occurrences of old_ip with new_ip."""
    try:
        with open(path) as f:
            lines = f.readlines()
        patched = [
            line.replace(old_ip, new_ip, 1) if line.startswith(old_ip) else line
            for line in lines
        ]
        if patched != lines:
            with open(path, 'w') as f:
                f.writelines(patched)
            log.info(f"Patched {os.path.basename(path)}: {old_ip} → {new_ip}")
    except OSError as e:
        log.error(f"_patch_file_ips {path}: {e}")


def _write_ip_cache() -> None:
    """Write {instance_id: ip} cache for deploy.sh / sync-fleet-ips.sh to read.

    Preserves last-known IP for stopped instances so the next startup can detect
    the old→new IP change and patch fleet files correctly.
    """
    try:
        with open(DORMANT_IP_CACHE) as f:
            existing = json.load(f)
    except (OSError, json.JSONDecodeError):
        existing = {}
    cache = dict(existing)
    for iid, ip in _current_ips.items():
        if ip:
            cache[iid] = ip
        # stopped (ip=None): keep last-known entry so next start can diff old→new
    try:
        with open(DORMANT_IP_CACHE, 'w') as f:
            json.dump(cache, f, indent=2)
    except OSError as e:
        log.error(f"_write_ip_cache: {e}")


def _poll_for_ip(inst: dict, ec2, timeout_s: int = 300) -> str | None:
    """Poll describe_instances until PublicIpAddress is assigned; return it or None."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        time.sleep(10)
        try:
            resp = ec2.describe_instances(InstanceIds=[inst['instance_id']])
            ip = resp['Reservations'][0]['Instances'][0].get('PublicIpAddress')
            if ip:
                return ip
        except Exception as e:
            log.warning(f"{inst['name']}: poll_for_ip: {e}")
    log.warning(f"{inst['name']}: timed out waiting for public IP after {timeout_s}s")
    return None


def _init_ip_state(ec2_clients: dict) -> None:
    """Query current public IPs for all instances at startup; patch fleet files for any
    IP changes that happened while the monitor was down."""
    prev: dict[str, str] = {}
    try:
        with open(DORMANT_IP_CACHE) as f:
            prev = json.load(f)
    except OSError:
        pass

    for inst in INSTANCES:
        ec2 = ec2_clients[inst['name']]
        try:
            resp = ec2.describe_instances(InstanceIds=[inst['instance_id']])
            ip = resp['Reservations'][0]['Instances'][0].get('PublicIpAddress')
            _current_ips[inst['instance_id']] = ip
            old_ip = prev.get(inst['instance_id'])
            if ip and old_ip and old_ip != ip:
                log.info(f"{inst['name']}: IP changed while monitor was down ({old_ip} → {ip})")
                _patch_file_ips(FLEET_SERVER_IPS_TXT, old_ip, ip)
                _patch_file_ips(FLEET_RPC_PORTS_TXT, old_ip, ip)
        except Exception as e:
            log.warning(f"{inst['name']}: _init_ip_state: {e}")
            _current_ips[inst['instance_id']] = prev.get(inst['instance_id'])

    _write_ip_cache()
    log.info(f"IP state initialised: { {k: v for k, v in _current_ips.items() if v} }")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def main() -> None:
    if not os.environ.get('AWS_ACCESS_KEY_ID'):
        log.warning("AWS_ACCESS_KEY_ID not set — EC2 calls will fail unless an instance role is attached.")

    ec2_clients = {
        inst['name']: boto3.client('ec2', region_name=inst['region'])
        for inst in INSTANCES
    }
    streaks = {inst['name']: 0 for inst in INSTANCES}
    _init_ip_state(ec2_clients)

    names = ', '.join(i['name'] for i in INSTANCES)
    log.info(f"Dormant instance monitor started. Instances: {names}")

    while True:
        try:
            guids_by_ip = active_guids_by_ip()
            total_guids = sum(len(g) for g in guids_by_ip.values())
            log.info(f"Census: {len(guids_by_ip)} servers, {total_guids} GUIDs in last {LOOKBACK_MINUTES}min. geo_cache={len(_geo_cache)}")

            thresholds = load_thresholds()
            for inst in INSTANCES:
                try:
                    thr = thresholds.get(inst['name'], inst['threshold'])
                    streaks[inst['name']] = check_instance(inst, ec2_clients, guids_by_ip, streaks[inst['name']], thr)
                except Exception as e:
                    log.error(f"{inst['name']}: check error: {e}", exc_info=True)

        except Exception as e:
            log.error(f"Cycle error: {e}", exc_info=True)

        log.info(f"Sleeping {CHECK_INTERVAL // 60} min.")
        time.sleep(CHECK_INTERVAL)


if __name__ == '__main__':
    main()
