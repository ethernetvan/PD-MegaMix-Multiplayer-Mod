"""Run against a test server and an idle PC at song select. No OS input."""
import argparse
import json
from pathlib import Path
import re
import secrets
import socket
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--admin-port", type=int, required=True)
    ap.add_argument("--server-log", type=Path, required=True)
    ap.add_argument("--cid", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--pv", type=int)
    ap.add_argument("--rounds", type=int, choices=(1, 2), default=1)
    args = ap.parse_args()
    if not args.cid.strip():
        ap.error("A non-empty target CID is required")
    host = "127.0.0.1"

    def admin(cmd, **fields):
        with socket.create_connection((host, args.admin_port), timeout=5) as s:
            s.sendall((json.dumps(dict(cmd=cmd, **fields)) + "\n").encode())
            return s.makefile("rb").readline().decode().strip()

    if any(m["cid"] == args.cid for l in json.loads(admin("lobbies")).values()
           for m in l["members"]):
        raise RuntimeError("Test client must not already be in a room")
    with socket.create_connection((host, args.port), timeout=5) as peer, \
            args.server_log.open("r", encoding="utf-8", errors="replace") as log:
        quoted = f"'{args.name}'"
        name = re.escape(quoted)
        latest = None
        for line in log:
            match = re.search(rf"SEL {name} .*state=(\d+) pv=(-?\d+)", line)
            if match:
                latest = int(match[2]) if match[1] == "6" else None
            elif f"SEL {quoted} DEST" in line:
                latest = None
        if latest is None:
            raise RuntimeError("Target must be browsing song select before the test")
        pv = args.pv if args.pv is not None else (637 if latest == 28 else 28)
        if pv == latest:
            raise ValueError("Choose a different PV to verify the PC follows the picker")

        def send(t, **fields):
            peer.sendall((json.dumps(dict(t=t, **fields)) + "\n").encode())

        def expect(pattern, timeout=30):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                line = log.readline()
                if not line:
                    time.sleep(0.1)
                elif f"MSG {quoted}" in line and any(
                    f'"ev":"{event}"' in line
                    for event in ("retry_detected", "exit_timeout", "drive_give_up")
                ):
                    raise RuntimeError(line.strip())
                elif re.search(pattern, line):
                    print(line.strip(), flush=True)
                    return
            raise RuntimeError(f"Did not observe {pattern!r} within {timeout}s")

        def member(predicate, timeout=60):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                members = json.loads(admin("lobbies"))[code]["members"]
                if any(m["cid"] == args.cid and predicate(m) for m in members):
                    return
                time.sleep(0.2)
            raise RuntimeError("Test client did not reach expected member state")

        send("hello", proto=1, mod="return-test", game="1.0.6",
             cid=secrets.token_hex(8), name="ReturnTestOwner")
        send("lobby_create")
        with peer.makefile("rb") as reader:
            while True:
                msg = json.loads(reader.readline())
                if msg.get("t") == "lobby":
                    code = msg["code"]
                    break
        try:
            if not admin("lobby_join", cid=args.cid, code=code).startswith("ok 1"):
                raise RuntimeError("Test client not connected")
            member(lambda m: m["connected"])
            send("sel", pv=pv)
            expect(rf"SEL {name} .*state=6 pv={pv}\b")
            for rnd in range(1, args.rounds + 1):
                if rnd == 1:
                    send("start_req", pv=pv)
                else:
                    if json.loads(admin("lobbies"))[code]["leader"] != args.cid:
                        raise RuntimeError("Picker did not rotate to the PC")
                    for state in (8, 10):
                        time.sleep(1)
                        admin("tap", cid=args.cid, btn="A", count=1, hold=80, gap=80)
                        expect(rf"SEL {name} .*state={state}\b")
                member(lambda m: m["loaded"] and m["role"] == "player")
                send("loaded", round=rnd)
                expect(rf"lobby {code}: start r{rnd} dropped=\[\]", 60)
                send("pause_req", round=rnd)
                expect(rf"DBG {name} .*paused=1")
                send("resume_req", round=rnd)
                expect(rf"DBG {name} .*loaded=1 paused=0")
                send("final", round=rnd, score=100, pct=1.0)
                expect(rf"lobby {code}: r{rnd} standings complete", 300)
                send("back_in_sel", round=rnd)
                expect(rf"lobby {code}: r{rnd} end ->")
                expect(rf"SEL {name} INIT", 60)
                expect(rf"SEL {name} .*state=6")
        finally:
            admin("lobby_leave", cid=args.cid)
            send("lobby_leave")


if __name__ == "__main__":
    main()
