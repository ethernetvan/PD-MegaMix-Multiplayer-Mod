#!/usr/bin/env python3
"""admin.py - send debug commands to the mp server admin port (forwards to clients)."""
import argparse, json, socket, sys

def send(payload, port):
    payload.setdefault("t", "dbg_cmd")
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall((json.dumps(payload) + "\n").encode())
        s.shutdown(socket.SHUT_WR)
        resp = s.recv(4096).decode(errors="replace").strip()
    print(resp)

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("tap"); p.add_argument("btn"); p.add_argument("--hold", type=int, default=50); p.add_argument("--gap", type=int, default=50); p.add_argument("--count", type=int, default=1)
    p = sub.add_parser("spam"); p.add_argument("btn", nargs="?"); p.add_argument("--hz", type=int, default=8); p.add_argument("--secs", type=int, default=60)
    sub.add_parser("block").add_argument("btn")
    sub.add_parser("unblock")
    sub.add_parser("jump").add_argument("pv", type=int)
    sub.add_parser("autopause").add_argument("state", choices=["on", "off"])
    sub.add_parser("list_pvs")
    sub.add_parser("spamstop")
    sub.add_parser("lobbies")
    sub.add_parser("kick").add_argument("kick_cid")
    p = sub.add_parser("set_leader"); p.add_argument("code"); p.add_argument("leader_cid")
    sub.add_parser("overlay").add_argument("state", choices=["open", "close"])
    p = sub.add_parser("cursor"); p.add_argument("x", type=int); p.add_argument("y", type=int); p.add_argument("--click", action="store_true")
    sub.add_parser("avatar_peek")
    sub.add_parser("avatar_poke").add_argument("module", type=int)
    sub.add_parser("avatar_dump").add_argument("module", type=int)
    ap.add_argument("--cid", default=None)
    ap.add_argument("--port", type=int, default=7790)
    args = ap.parse_args()

    pl = {}
    if args.cmd == "tap":
        pl = {"cmd": "tap", "btn": args.btn, "hold": args.hold, "gap": args.gap, "count": args.count}
    elif args.cmd == "spam":
        if args.btn in (None, "stop"):
            pl = {"cmd": "spam", "stop": True}
        else:
            pl = {"cmd": "spam", "btn": args.btn, "hz": args.hz, "secs": args.secs}
    elif args.cmd == "spamstop":
        pl = {"cmd": "spam", "stop": True}
    elif args.cmd == "block":
        pl = {"cmd": "block", "btn": args.btn}
    elif args.cmd == "unblock":
        pl = {"cmd": "unblock"}
    elif args.cmd == "jump":
        pl = {"cmd": "jump", "pv": args.pv}
    elif args.cmd == "autopause":
        pl = {"cmd": "autopause", "on": args.state == "on"}
    elif args.cmd == "list_pvs":
        pl = {"cmd": "list_pvs"}
    elif args.cmd == "lobbies":
        pl = {"cmd": "lobbies"}
    elif args.cmd == "kick":
        pl = {"cmd": "kick", "cid": args.kick_cid}
    elif args.cmd == "set_leader":
        pl = {"cmd": "set_leader", "code": args.code, "cid": args.leader_cid}
    elif args.cmd == "overlay":
        pl = {"cmd": "overlay", "open": args.state == "open"}
    elif args.cmd == "cursor":
        pl = {"cmd": "cursor", "x": args.x, "y": args.y, "click": args.click}
    elif args.cmd == "avatar_peek":
        pl = {"cmd": "peek_avatar"}
    elif args.cmd == "avatar_poke":
        pl = {"cmd": "poke_avatar", "module": args.module}
    elif args.cmd == "avatar_dump":
        pl = {"cmd": "dump_avatar", "module": args.module}
    if args.cid:
        pl["cid"] = args.cid
    send(pl, args.port)

if __name__ == "__main__":
    main()
