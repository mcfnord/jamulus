#!/usr/bin/env python3
"""
GUID-IP association quality investigation.

Reads join-events.csv and characterizes:
1. Strength distribution across all GUIDs
2. How many GUIDs reach high-confidence threshold (>=16)
3. IP consistency: do high-confidence GUIDs get the same IP across sessions?
4. Server-vs-player region mismatch: for GUIDs with a known IP, how often does
   the server they played on share the same region?

join-events.csv columns (0-indexed):
  0=timestamp, 1=server_ip:port, 2=guid, 3=name, 4=instrument, 5=?,
  6=country, 7=city, 8=?, 9=lat, 10=lon, 11=client_ip:port, 12=strength, 13=ASN
"""

import csv
import sys
import json
import time
import urllib.request
from collections import defaultdict
from pathlib import Path

JOIN_EVENTS = Path("/root/JamFan22/JamFan22/join-events.csv")
HIGH_CONF_THRESHOLD = 16
IP_API_BATCH = 100  # ip-api free tier: 45 req/min; we batch and cache


def load_join_events():
    rows = []
    with open(JOIN_EVENTS, newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) < 13:
                continue
            rows.append(row)
    return rows


def geolocate_ips(ips):
    """Batch geolocate a list of IPs using ip-api.com. Returns {ip: regionName}."""
    result = {}
    ips = list(set(ips))
    for i in range(0, len(ips), IP_API_BATCH):
        batch = ips[i:i + IP_API_BATCH]
        payload = json.dumps([{"query": ip} for ip in batch]).encode()
        req = urllib.request.Request(
            "http://ip-api.com/batch?fields=query,regionName,country,status",
            data=payload,
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read())
            for item in data:
                if item.get("status") == "success":
                    result[item["query"]] = {
                        "region": item.get("regionName", ""),
                        "country": item.get("country", ""),
                    }
        except Exception as e:
            print(f"  [geo] batch {i//IP_API_BATCH} failed: {e}", file=sys.stderr)
        if i + IP_API_BATCH < len(ips):
            time.sleep(1.5)  # stay under rate limit
    return result


def main():
    print(f"Loading {JOIN_EVENTS} ...")
    rows = load_join_events()
    print(f"  {len(rows):,} rows loaded")

    # Per-GUID stats
    guid_max_strength = defaultdict(int)
    guid_ips = defaultdict(set)           # non-empty client IPs seen
    guid_servers = defaultdict(set)       # server IPs seen (col 1, strip port)
    guid_names = defaultdict(set)

    for row in rows:
        guid = row[2].strip()
        if not guid:
            continue
        name = row[3].strip()
        server = row[1].strip().split(":")[0]
        client_ip_port = row[11].strip()
        try:
            strength = int(row[12].strip())
        except ValueError:
            strength = 0

        guid_max_strength[guid] = max(guid_max_strength[guid], strength)
        if client_ip_port:
            ip = client_ip_port.split(":")[0]
            guid_ips[guid].add(ip)
        if server:
            guid_servers[guid].add(server)
        if name:
            guid_names[guid].add(name)

    total_guids = len(guid_max_strength)
    print(f"\n=== GUID count: {total_guids:,} ===\n")

    # Strength distribution
    buckets = [(0,0), (1,3), (4,7), (8,11), (12,15), (16,31), (32,63), (64, 9999)]
    print("Strength distribution:")
    for lo, hi in buckets:
        count = sum(1 for s in guid_max_strength.values() if lo <= s <= hi)
        bar = "#" * (count * 40 // total_guids) if total_guids else ""
        print(f"  {lo:3}–{hi:<4}  {count:5,}  {bar}")

    high_conf = {g for g, s in guid_max_strength.items() if s >= HIGH_CONF_THRESHOLD}
    print(f"\nHigh-confidence GUIDs (strength >= {HIGH_CONF_THRESHOLD}): {len(high_conf):,}"
          f"  ({100*len(high_conf)/total_guids:.1f}%)")

    # IP consistency for high-confidence GUIDs
    print("\n=== IP consistency (high-confidence GUIDs) ===")
    single_ip = sum(1 for g in high_conf if len(guid_ips[g]) == 1)
    multi_ip  = sum(1 for g in high_conf if len(guid_ips[g]) > 1)
    no_ip     = sum(1 for g in high_conf if len(guid_ips[g]) == 0)
    print(f"  1 IP (consistent):   {single_ip:4,}")
    print(f"  2+ IPs (varies):     {multi_ip:4,}")
    print(f"  No IP (all empty):   {no_ip:4,}")

    # Collect IPs to geolocate
    all_ips_to_geo = set()
    for g in high_conf:
        all_ips_to_geo.update(guid_ips[g])
    all_server_ips = set()
    for g in high_conf:
        all_server_ips.update(guid_servers[g])
    all_ips_to_geo.update(all_server_ips)

    if not all_ips_to_geo:
        print("\nNo IPs to geolocate — exiting.")
        return

    print(f"\nGeolocating {len(all_ips_to_geo):,} unique IPs (client + server) ...")
    geo = geolocate_ips(list(all_ips_to_geo))
    print(f"  Resolved: {len(geo):,}")

    # Server-vs-player region mismatch
    print("\n=== Server region vs. player region (high-confidence GUIDs with 1 client IP) ===")
    agree = disagree = no_data = 0
    mismatch_examples = []

    for guid in high_conf:
        if len(guid_ips[guid]) != 1:
            continue
        client_ip = next(iter(guid_ips[guid]))
        player_geo = geo.get(client_ip)
        if not player_geo:
            no_data += 1
            continue
        player_region = player_geo["region"]
        player_country = player_geo["country"]

        server_regions = set()
        for sip in guid_servers[guid]:
            sg = geo.get(sip)
            if sg:
                server_regions.add((sg["region"], sg["country"]))

        if not server_regions:
            no_data += 1
            continue

        # Does any server share the player's country?
        same_country = any(c == player_country for _, c in server_regions)
        # Does any server share the player's region?
        same_region  = any(r == player_region and c == player_country
                           for r, c in server_regions)

        if same_region:
            agree += 1
        elif same_country:
            agree += 1  # count same-country as agreement for this analysis
            disagree_detail = "same country, diff region"
            mismatch_examples.append(
                (list(guid_names[guid])[:1], client_ip, player_region, player_country,
                 server_regions, disagree_detail)
            )
        else:
            disagree += 1
            mismatch_examples.append(
                (list(guid_names[guid])[:1], client_ip, player_region, player_country,
                 server_regions, "different country")
            )

    total_checked = agree + disagree
    print(f"  Agree (same country/region): {agree:4,}")
    print(f"  Disagree (server in diff country): {disagree:4,}")
    print(f"  No geo data: {no_data:4,}")
    if total_checked:
        print(f"  Agreement rate: {100*agree/total_checked:.1f}%")

    if mismatch_examples:
        print(f"\nMismatch examples (up to 10):")
        for name, ip, region, country, server_regions, detail in mismatch_examples[:10]:
            srv = ", ".join(f"{r}/{c}" for r, c in list(server_regions)[:3])
            print(f"  {name} | player={region},{country} | servers={srv} | {detail}")

    # Bonus: show top GUIDs by max strength
    print("\n=== Top 15 GUIDs by max strength ===")
    top = sorted(guid_max_strength.items(), key=lambda x: -x[1])[:15]
    for guid, strength in top:
        names = ", ".join(sorted(guid_names[guid]))[:40]
        ips = ", ".join(sorted(guid_ips[guid]))
        print(f"  str={strength:3}  {guid[:12]}  {names:<40}  ips={ips}")


if __name__ == "__main__":
    main()
