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
    p = sub.add_parser("autoplay"); p.add_argument("state", choices=["on", "off"]); p.add_argument("--pct", type=int, default=None)
    p.add_argument("--mode", choices=["auto", "pro", "97", "streak", "human", "clutch", "tie"], default=None)
    p = sub.add_parser("glow"); p.add_argument("dir", choices=["up", "down"])
    p = sub.add_parser("notify_style"); p.add_argument("style", choices=["avatars", "text", "off"])
    sub.add_parser("popup_test")
    p = sub.add_parser("menu_holds")
    for k in ("init", "score_a", "vp", "menu", "nav"):
        p.add_argument("--" + k, type=int, default=-1)
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
    sub.add_parser("module_dump")
    sub.add_parser("avatar_hide").add_argument("target_cid")
    sub.add_parser("remote_avatars").add_argument("state", choices=["on", "off"])
    sub.add_parser("server_mode").add_argument("mode", choices=["off", "local", "cloud"])
    sub.add_parser("cloud_url").add_argument("url", help='host[:port][/path]; "" = default')
    sub.add_parser("menu_cursor").add_argument("v", type=int)
    p = sub.add_parser("sel_lists"); p.add_argument("pvs", type=int, nargs="+")
    sub.add_parser("lobby_create")
    sub.add_parser("lobby_join").add_argument("code")
    sub.add_parser("lobby_leave")
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
    elif args.cmd == "autoplay":
        pl = {"cmd": "autoplay", "on": args.state == "on"}
        if args.pct is not None:
            pl["pct"] = max(0, min(100, args.pct))
        if args.mode is not None:
            pl["mode"] = args.mode
    elif args.cmd == "menu_holds":
        pl = {"cmd": "menu_holds", "init": args.init,
              "score_a": args.score_a, "vp": args.vp,
              "menu": args.menu, "nav": args.nav}
    elif args.cmd == "glow":
        pl = {"cmd": "glow", "dir": args.dir}
    elif args.cmd == "notify_style":
        pl = {"cmd": "notify_style", "style": args.style}
    elif args.cmd == "popup_test":
        pl = {"cmd": "popup_test"}
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
    elif args.cmd == "module_dump":
        pl = {"cmd": "module_dump"}
    elif args.cmd == "avatar_hide":
        pl = {"cmd": "avatar_hide", "target": args.target_cid}
    elif args.cmd == "remote_avatars":
        pl = {"cmd": "remote_avatars", "on": args.state == "on"}
    elif args.cmd == "cloud_url":
        pl = {"cmd": "cloud_url", "url": args.url}
    elif args.cmd == "server_mode":
        pl = {"cmd": "server_mode", "mode": args.mode}
    elif args.cmd == "menu_cursor":
        pl = {"cmd": "menu_cursor", "v": args.v}
    elif args.cmd == "sel_lists":
        pl = {"cmd": "sel_lists", "pvs": args.pvs}
    elif args.cmd in ("lobby_create", "lobby_leave"):
        pl = {"cmd": args.cmd}
    elif args.cmd == "lobby_join":
        pl = {"cmd": "lobby_join", "code": args.code.upper()}
    elif args.cmd == "glow":
        pl = {"cmd": "glow", "dir": args.dir}
    if args.cid:
        pl["cid"] = args.cid
    send(pl, args.port)

if __name__ == "__main__":
    main()
