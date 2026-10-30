#!/usr/bin/env python3
"""fake_client.py - protocol-v1 fake game client for testing mp_server.

Usage:
  python fake_client.py [--host X] [--port 7777] [--discovery-port 7778]
                        [--name NAME] [--cid CID] [--dbg] [--pings N]
Without --host it tries UDP discovery first.
"""

import argparse
import json
import socket
import sys
import time


def discover(disc_port, timeout=3.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    probe = json.dumps({"t": "discover", "proto": 1}) + "\n"
    s.sendto(probe.encode(), ("255.255.255.255", disc_port))
    try:
        data, addr = s.recvfrom(4096)
        msg = json.loads(data.decode().strip())
        if msg.get("t") == "server":
            print(f"discovered {addr[0]}:{msg['port']} name={msg.get('name')}")
            return addr[0], msg["port"]
    except socket.timeout:
        pass
    finally:
        s.close()
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--discovery-port", type=int, default=7778)
    ap.add_argument("--name", default="FakeClient")
    ap.add_argument("--cid", default="deadbeefcafe0001")
    ap.add_argument("--dbg", action="store_true", help="send fake dbg_state")
    ap.add_argument("--pings", type=int, default=4)
    args = ap.parse_args()

    host, port = args.host, args.port
    if host is None:
        host, port = discover(args.discovery_port)
        if host is None:
            print("discovery failed; use --host")
            sys.exit(1)

    s = socket.create_connection((host, port), timeout=5)
    f = s.makefile("rw")

    hello = {"t": "hello", "proto": 1, "mod": "0.1.0-dev+fake", "game": "1.0.6",
             "cid": args.cid, "name": args.name}
    f.write(json.dumps(hello) + "\n")
    f.flush()
    print("<-", f.readline().strip())

    for i in range(1, args.pings + 1):
        f.write(json.dumps({"t": "ping", "seq": i, "ct": int(time.time() * 1000)}) + "\n")
        f.flush()
        print("<-", f.readline().strip())
        if args.dbg:
            dbg = {"t": "dbg_state", "f": i, "loaded": True, "paused": False,
                   "score": i * 1000, "combo": i, "max_combo": i, "life": 100,
                   "pct": 12.5 * i, "jc": [i, 0, 0, 0, 0], "scoring": True,
                   "type": 3, "diff": 2, "pv": 10 + i}
            f.write(json.dumps(dbg) + "\n")
            f.flush()
        time.sleep(0.5)

    s.close()
    print("done")


if __name__ == "__main__":
    main()
