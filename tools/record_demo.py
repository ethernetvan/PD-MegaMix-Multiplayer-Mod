#!/usr/bin/env python3
"""record_demo.py - record a scripted MegaMix multiplayer demo video.

Boots the game in Ryujinx (fullscreen, monitor 0), screen-records it with
ffmpeg (ddagrab + dshow audio), drives a scripted tour of the multiplayer
UI plus one live round against bots, and produces a captioned, chaptered
MP4.

Self-contained: needs only Python 3.13 stdlib + ffmpeg/ffprobe on PATH.

Usage:
    python tools/record_demo.py                # full run
    python tools/record_demo.py --skip-tour    # room + bots, straight to the round
"""

import argparse
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_DIR = os.path.join(REPO, "server")
BOT_PY = os.path.join(SERVER_DIR, "bot.py")
SERVER_PY = os.path.join(SERVER_DIR, "mp_server.py")

RYUJINX_EXE = r"C:\Users\cl672\Documents\Juegos\Ryujinx\Ryujinx.exe"
RYUJINX_LOGS = r"C:\Users\cl672\Documents\Juegos\Ryujinx\Logs"
NSP = r"C:\Users\cl672\Documents\Juegos\switchgames\Hatsune_Miku_Merged.nsp"
SD_ROOT = os.path.join(os.environ.get("APPDATA", ""), "Ryujinx", "sdcard")
CONFIG_TOML = os.path.join(SD_ROOT, "MikuMikuSwitchPlugin", "config.toml")
OUT_DIR = r"C:\Users\cl672\Documents\daniel-gdrive\pdmm"
WORK_ROOT = os.path.join(tempfile.gettempdir(), "megamix_demo")

AUDIO_DEV = "Stereo Mix (Realtek(R) Audio)"

# ----------------------------- 720p UI coordinates --------------------------
MP_BADGE = (1180, 92)
CARD_EDIT_NAME = (294, 362)
CARD_CREATE = (640, 360)
CARD_JOIN = (985, 362)
DLG_CANCEL = (380, 274)
DLG_OK = (897, 274)          # SAVE / JOIN
ROOM_CONFIG = (790, 90)
ROOM_EXIT = (993, 90)
ROOM_CLOSE_X = (1135, 90)
RC_ROTATE = (296, 263)
RC_OWNER = (640, 263)
RC_ASSIGNED = (983, 263)
RC_DONE = (1072, 90)
KICK_X = 1120
KICK_Y0, KICK_DY = 171, 56
GRID_X = (381, 897)
GRID_Y0, GRID_DY = 403, 48
KB_CAPS = (89, 575)
KB_CLR = (1183, 575)
KB_ROW1 = {"q": 96, "w": 180, "e": 264, "r": 347, "t": 430,
           "y": 513, "u": 597, "i": 681, "o": 764, "p": 847}
KB_ROW2 = {"a": 167, "s": 251, "d": 336, "f": 421, "g": 507,
           "h": 592, "j": 676, "k": 761, "l": 847}
KB_ROW3 = {"z": 104, "x": 201, "c": 298, "v": 395,
           "b": 493, "n": 590, "m": 687}
KB_KEYS = {c: (x, 531) for c, x in KB_ROW1.items()}
KB_KEYS.update({c: (x, 575) for c, x in KB_ROW2.items()})
KB_KEYS.update({c: (x, 619) for c, x in KB_ROW3.items()})

BOT_NAMES = ["Rin", "Len", "Luka", "Kaito", "Neru"]
# demo song + fallbacks: Ievan Polkka (~2:27), Blackjack (~2:41),
# The World is Mine (~2:50). Note: 637 Senbonzakura -F edition- measured
# ~3:38 in-game - too long for the demo.
SONG_CANDIDATES = [700, 701, 738, 2]


class DemoError(Exception):
    pass


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def warn(msg):
    print(f"[{time.strftime('%H:%M:%S')}] !! {msg}", flush=True)


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def newest_ryu_log():
    """(path, mtime, size) of the newest Ryujinx log, or None."""
    files = glob.glob(os.path.join(RYUJINX_LOGS, "Ryujinx_*.log"))
    if not files:
        return None
    p = max(files, key=os.path.getmtime)
    st = os.stat(p)
    return p, st.st_mtime, st.st_size


class Tail:
    """Incrementally reads a growing text file; wait() consumes matches."""

    def __init__(self, path):
        self.path = path
        self.pos = 0
        self.buf = ""

    def _read(self):
        """Append new file content to self.buf."""
        try:
            with open(self.path, encoding="utf-8", errors="replace") as f:
                f.seek(self.pos)
                self.buf += f.read()
                self.pos = f.tell()
        except OSError:
            pass

    def wait(self, pattern, timeout, desc=None):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            self._read()
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            if time.time() >= deadline:
                raise DemoError(
                    f"timeout ({timeout}s) waiting for "
                    f"{desc or pattern!r} in {os.path.basename(self.path)}")
            time.sleep(0.12)


class Admin:
    """Persistent connection to the mp_server admin port."""

    def __init__(self, port, cid):
        self.port = port
        self.cid = cid
        self.sock = None
        self.rf = None

    def _conn(self):
        if self.sock is None:
            self.sock = socket.create_connection(
                ("127.0.0.1", self.port), timeout=5)
            self.sock.settimeout(8)
            self.rf = self.sock.makefile("r", encoding="utf-8",
                                         errors="replace")

    def cmd(self, payload):
        payload = dict(payload)
        payload.setdefault("t", "dbg_cmd")
        payload["cid"] = self.cid
        for _ in range(2):
            try:
                self._conn()
                self.sock.sendall((json.dumps(payload) + "\n").encode())
                line = self.rf.readline()
                if line:
                    return line.strip()
            except OSError:
                pass
            self.close()
        raise DemoError("admin port did not respond")

    def lobbies(self):
        return json.loads(self.cmd({"cmd": "lobbies"}))

    def tap(self, btn, hold=60, gap=60, count=1):
        return self.cmd({"cmd": "tap", "btn": btn, "hold": hold,
                         "gap": gap, "count": count})

    def cursor(self, x, y, click=False):
        return self.cmd({"cmd": "cursor", "x": int(round(x)),
                         "y": int(round(y)), "click": bool(click)})

    def jump(self, pv):
        return self.cmd({"cmd": "jump", "pv": int(pv)})

    def overlay(self, open_):
        return self.cmd({"cmd": "overlay", "open": bool(open_)})

    def raw(self, payload):
        return self.cmd(payload)

    def close(self):
        for s in (self.rf, self.sock):
            try:
                s.close()
            except Exception:
                pass
        self.sock = None
        self.rf = None


class Bot:
    def __init__(self, demo, name, join=None, create=False):
        args = [sys.executable, "-u", "-B", BOT_PY,
                "--name", name, "--port", str(demo.port),
                "--behavior", "follower",
                "--song-secs", str(demo.song_secs),
                "--load-delay", str(demo.bot_load_delay)]
        if join:
            args += ["--join", join]
        if create:
            args += ["--create"]
        self.name = name
        self.buf = ""
        self.lock = threading.Lock()
        self.logf = open(os.path.join(demo.workdir, f"bot_{name}.log"),
                         "w", encoding="utf-8", errors="replace")
        self.proc = subprocess.Popen(
            args, cwd=SERVER_DIR, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", bufsize=1)
        demo.track(self.proc, f"bot {name}")
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        try:
            for line in self.proc.stdout:
                with self.lock:
                    self.buf += line
                try:
                    self.logf.write(line)
                    self.logf.flush()
                except Exception:
                    pass
        except Exception:
            pass

    def send(self, line):
        try:
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()
        except OSError:
            pass

    def wait(self, pattern, timeout, desc=None):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            with self.lock:
                m = rx.search(self.buf)
                if m:
                    self.buf = self.buf[m.end():]
                    return m
            if time.time() >= deadline:
                raise DemoError(
                    f"timeout ({timeout}s) waiting for {desc or pattern!r} "
                    f"in bot {self.name} output")
            time.sleep(0.1)


class Recorder:
    """ffmpeg ddagrab (+dshow audio) -> raw.mkv; rec_t0 = first frame."""

    def __init__(self, demo):
        self.d = demo
        self.proc = None
        self.rec_t0 = None
        self.stopped_at = None
        self.with_audio = not demo.no_audio
        self.err_log = os.path.join(demo.workdir, "ffmpeg_record.log")

    def _argv(self, with_audio):
        d = self.d
        a = [d.ffmpeg, "-hide_banner", "-y",
             "-f", "lavfi", "-i",
             f"ddagrab=output_idx={d.monitor}:framerate={d.fps}"
             f":draw_mouse=0,hwdownload,format=bgra",
             "-thread_queue_size", "1024"]
        if with_audio:
            a += ["-f", "dshow", "-audio_buffer_size", "50",
                  "-i", f"audio={d.audio_device}",
                  "-map", "0:v", "-map", "1:a"]
        else:
            a += ["-map", "0:v"]
        a += d.enc_args
        if with_audio:
            a += ["-af", "aresample=async=1",
                  "-c:a", "aac", "-b:a", "192k"]
        a += ["-nostats", "-progress", "pipe:1",
              os.path.join(d.workdir, "raw.mkv")]
        return a

    def start(self):
        self.logf = open(self.err_log, "w", encoding="utf-8",
                         errors="replace")
        for attempt in range(2):
            self._spawn(self.with_audio)
            t0 = time.time()
            while time.time() - t0 < 20 and self.rec_t0 is None \
                    and self.proc.poll() is None:
                time.sleep(0.1)
            if self.rec_t0 is not None:
                break
            self.stop()
            if self.with_audio:
                warn(f"capture failed with audio device "
                     f"{self.d.audio_device!r} - retrying without audio")
                self.with_audio = False
            else:
                break
        if self.rec_t0 is None:
            tail = ""
            try:
                with open(self.err_log, errors="replace") as f:
                    tail = f.read()[-1500:]
            except OSError:
                pass
            raise DemoError("ffmpeg capture did not start "
                            f"(no first frame). ffmpeg log tail:\n{tail}")
        log(f"recording started (audio={'on' if self.with_audio else 'off'})")

    def _spawn(self, with_audio):
        argv = self._argv(with_audio)
        self.logf.write("$ " + " ".join(argv) + "\n")
        self.logf.flush()
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.logf, text=True, encoding="utf-8",
            errors="replace", bufsize=1)
        self.d.track(self.proc, "ffmpeg record")
        threading.Thread(target=self._progress, daemon=True).start()

    def _progress(self):
        try:
            for line in self.proc.stdout:
                if self.rec_t0 is None and re.match(r"frame=\s*[1-9]", line):
                    self.rec_t0 = time.time()
        except Exception:
            pass

    def stop(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        try:
            self.proc.stdin.write("q\n")
            self.proc.stdin.flush()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=10)
        self.stopped_at = time.time()


# ---------------------------------------------------------------------------
# demo driver
# ---------------------------------------------------------------------------

class Demo:
    def __init__(self, a):
        self.a = a
        self.ryujinx = a.ryujinx
        self.nsp = a.nsp
        self.port = a.port_base
        self.disc_port = a.port_base + 1
        self.admin_port = a.port_base + 13
        self.monitor = a.monitor
        self.fps = a.fps
        self.audio_device = a.audio_device
        self.no_audio = a.no_audio
        self.ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
        self.ffprobe = shutil.which("ffprobe") or "ffprobe"
        self.song = a.song
        self.song_secs = a.song_secs
        self.bot_load_delay = a.bot_load_delay
        self.workdir = a.work_dir or os.path.join(
            WORK_ROOT, datetime.now().strftime("%Y%m%d-%H%M%S"))
        self.out_dir = a.out_dir
        self.skip_tour = a.skip_tour
        self.keep_raw = a.keep_raw
        self.frames_dir = a.frames_dir
        self.boot_wait = a.boot_wait
        self.procs = []
        self.bots = []
        self.captions = []       # (t_sec, text)
        self.rec = None
        self.cursor_pos = (640, 360)
        self.admin = None
        self.slog = None
        self.client_cid = None
        self.cfg_backup = None
        self.cfg_patched = False
        self.ryu = None
        self.encoder = None
        self.enc_args = []
        self.video_end = 0.0
        self.out_mp4 = None

    # ---------------------------------------------------- infrastructure
    def track(self, proc, what):
        self.procs.append((proc, what))

    def kill_tracked(self):
        for proc, what in self.procs:
            if proc.poll() is None:
                try:
                    proc.kill()
                except Exception:
                    pass
        for proc, what in self.procs:
            try:
                proc.wait(timeout=5)
            except Exception:
                pass

    def mark(self, caption):
        if self.rec and self.rec.rec_t0:
            t = time.time() - self.rec.rec_t0
        else:
            t = 0.0
        self.captions.append((max(0.0, t), caption))
        log(f"  >> {caption}")

    def hold(self, sec):
        time.sleep(sec)

    # ---- cursor (720p coords, human-ish glide)
    def glide(self, x, y, dur=0.4, steps=10):
        x0, y0 = self.cursor_pos
        for i in range(1, steps + 1):
            self.admin.cursor(x0 + (x - x0) * i / steps,
                              y0 + (y - y0) * i / steps)
            time.sleep(dur / steps)
        self.cursor_pos = (x, y)

    def click(self, x, y, glide=0.4, settle=0.35):
        self.glide(x, y, dur=glide)
        time.sleep(0.12)
        self.admin.cursor(x, y, click=True)
        time.sleep(settle)

    def type_text(self, text):
        for ch in text:
            pos = KB_KEYS.get(ch.lower())
            if pos is None:
                raise DemoError(f"no keyboard key mapped for {ch!r}")
            self.click(pos[0], pos[1], glide=0.12, settle=0.10)

    # ---------------------------------------------------- preflight
    def preflight(self):
        for p in (self.ryujinx, self.nsp, SERVER_PY, BOT_PY, CONFIG_TOML):
            if not os.path.exists(p):
                raise DemoError(f"missing required file: {p}")
        for tool in (self.ffmpeg, self.ffprobe):
            r = subprocess.run([tool, "-hide_banner", "-version"],
                               capture_output=True, timeout=15)
            if r.returncode != 0:
                raise DemoError(f"{tool} not usable")
        # isolated ports must be free
        for port, proto in ((self.port, "tcp"), (self.admin_port, "tcp"),
                            (self.disc_port, "udp")):
            s = socket.socket(socket.AF_INET,
                              socket.SOCK_DGRAM if proto == "udp"
                              else socket.SOCK_STREAM)
            try:
                s.bind(("0.0.0.0" if proto == "udp" else "127.0.0.1", port))
            except OSError:
                s.close()
                raise DemoError(f"port {port}/{proto} is already in use - "
                                "pick another --port-base")
            s.close()
        # NVENC preflight (1-frame encode), fallback libx264
        r = subprocess.run(
            [self.ffmpeg, "-hide_banner", "-y", "-f", "lavfi", "-i",
             "color=c=black:s=256x256:d=0.2:r=30",
             "-c:v", "h264_nvenc", "-f", "null", "-"],
            capture_output=True, timeout=30)
        if r.returncode == 0:
            self.encoder = "h264_nvenc"
            self.enc_args = ["-c:v", "h264_nvenc", "-preset", "p5",
                             "-rc", "vbr", "-cq", "21", "-b:v", "0",
                             "-pix_fmt", "yuv420p"]
        else:
            warn("h264_nvenc preflight failed - using libx264")
            self.encoder = "libx264"
            self.enc_args = ["-c:v", "libx264", "-preset", "veryfast",
                             "-crf", "22", "-pix_fmt", "yuv420p"]
        log(f"preflight ok (encoder={self.encoder}, ports "
            f"{self.port}/{self.disc_port}/{self.admin_port})")

    def kill_ryujinx(self):
        r = subprocess.run(["taskkill", "/IM", "Ryujinx.exe", "/F"],
                           capture_output=True, text=True)
        if "SUCCESS" in ((r.stdout or "") + (r.stderr or "")):
            warn("killed a running Ryujinx.exe (required before launch)")
            time.sleep(2)

    # ---------------------------------------------------- config.toml
    def patch_config(self):
        with open(CONFIG_TOML, "rb") as f:
            self.cfg_backup = f.read()
        text = self.cfg_backup.decode("utf-8")
        lines = text.split("\n")
        wanted = {
            "enabled": "true",
            "server_ip": '""',
            "server_port": str(self.port),
            "discovery_port": str(self.disc_port),
            "player_name": '"Player"',
        }
        in_mp = False
        mp_start = -1
        seen = set()
        for i, ln in enumerate(lines):
            s = ln.strip()
            if s.startswith("["):
                if in_mp:
                    break
                in_mp = s == "[multiplayer]"
                if in_mp:
                    mp_start = i
                continue
            if not in_mp:
                continue
            m = re.match(r"\s*(enabled|server_ip|server_port|"
                         r"discovery_port|player_name)\s*=", ln)
            if m:
                key = m.group(1)
                rest = ln[m.end():]
                cmt = re.search(r"#.*$", rest)
                lines[i] = f"{key} = {wanted[key]}" + \
                           ((" " + cmt.group(0)) if cmt else "")
                seen.add(key)
        if mp_start < 0:
            lines += ["", "[multiplayer]"] + \
                     [f"{k} = {v}" for k, v in wanted.items()]
        else:
            missing = [f"{k} = {v}" for k, v in wanted.items()
                       if k not in seen]
            lines[mp_start + 1:mp_start + 1] = missing
        with open(CONFIG_TOML, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(lines))
        self.cfg_patched = True
        m = re.search(r'client_id\s*=\s*"([^"]+)"', text)
        self.client_cid = m.group(1) if m else None
        log(f"config.toml patched (server :{self.port}, "
            f"discovery :{self.disc_port}, cid={self.client_cid})")

    def restore_config(self):
        if not self.cfg_patched or self.cfg_backup is None:
            return
        with open(CONFIG_TOML, "wb") as f:
            f.write(self.cfg_backup)
        with open(CONFIG_TOML, "rb") as f:
            if f.read() != self.cfg_backup:
                warn("config.toml restore verification FAILED")
                return
        self.cfg_patched = False
        log("config.toml restored byte-identical")

    # ---------------------------------------------------- processes
    def start_server(self):
        self.server_logf = open(os.path.join(self.workdir, "server.log"),
                                "w", encoding="utf-8", errors="replace")
        proc = subprocess.Popen(
            [sys.executable, "-u", "-B", SERVER_PY,
             "--port", str(self.port),
             "--discovery-port", str(self.disc_port),
             "--admin-port", str(self.admin_port)],
            cwd=SERVER_DIR, stdout=self.server_logf,
            stderr=subprocess.STDOUT)
        self.track(proc, "mp_server")
        self.slog = Tail(self.server_logf.name)
        self.slog.wait(r"listening TCP", 10, "server startup")
        self.admin = Admin(self.admin_port, self.client_cid)
        log(f"mp_server up on :{self.port}/udp:{self.disc_port}"
            f"/admin:{self.admin_port}")

    def launch_ryujinx(self):
        for i in range(5, 0, -1):
            print(f"  *** going fullscreen on monitor {self.monitor} "
                  f"in {i}s - do not use that display ***", flush=True)
            time.sleep(1)
        self._spawn_ryu()

    def _spawn_ryu(self):
        pre = newest_ryu_log()
        self.ryu_log_marker = pre[0] if pre else None
        self.ryu = subprocess.Popen(
            [self.ryujinx, "--fullscreen", self.nsp],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.track(self.ryu, "Ryujinx")
        self.ryu_launch_at = time.time()
        log("Ryujinx launched (fullscreen)")

    def _ryu_log_progress(self):
        """(path, size) of our instance's log or None if not created yet."""
        cur = newest_ryu_log()
        if not cur or cur[0] == self.ryu_log_marker:
            return None
        return cur[0], cur[2]

    def wait_client_hello(self):
        """Boot watchdog: log-stall/no-HELLO -> relaunch once -> fail."""
        cid = self.client_cid or ""
        hello_rx = re.compile(rf"HELLO {re.escape(cid)}\b") if cid \
            else re.compile(r"HELLO \S+ 'Player'")
        for attempt in (1, 2):
            deadline = time.time() + 90
            last_size, last_change = -1, time.time()
            while time.time() < deadline:
                prog = self._ryu_log_progress()
                if prog:
                    if prog[1] != last_size:
                        last_size, last_change = prog[1], time.time()
                    elif time.time() - last_change > 20:
                        warn(f"Ryujinx log stalled "
                             f"{time.time() - last_change:.0f}s during boot")
                        break
                else:
                    last_change = time.time()
                self.slog._read()
                if hello_rx.search(self.slog.buf):
                    log("client connected to our server (HELLO seen)")
                    return
                if self.ryu.poll() is not None:
                    warn("Ryujinx exited during boot")
                    break
                time.sleep(1.5)
            if attempt == 2:
                raise DemoError(
                    "game boot failed twice (no HELLO / stalled log) - "
                    "see server.log + newest Ryujinx log")
            warn("boot watchdog: relaunching Ryujinx")
            try:
                self.ryu.kill()
                self.ryu.wait(timeout=8)
            except Exception:
                pass
            self._spawn_ryu()

    # ---------------------------------------------------- lobby helpers
    def my_lobby(self):
        for code, l in self.admin.lobbies().items():
            if any(m["cid"] == self.client_cid for m in l["members"]):
                return code, l
        return None, None

    def wait_lobby(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            code, l = self.my_lobby()
            if l:
                return code, l
            time.sleep(0.4)
        raise DemoError("timeout waiting for client to be in a lobby")

    def wait_no_lobby(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, l = self.my_lobby()
            if not l:
                return
            time.sleep(0.4)
        raise DemoError("timeout waiting for client to leave the lobby")

    def wait_leader(self, cid, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, l = self.my_lobby()
            if l and l.get("leader") == cid:
                return l
            time.sleep(0.4)
        raise DemoError(f"timeout waiting for leader == {cid}")

    def wait_members(self, n, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, l = self.my_lobby()
            if l and len(l["members"]) >= n:
                return l
            time.sleep(0.5)
        raise DemoError(f"timeout waiting for {n} lobby members")

    def member_index(self, lobby, name):
        for i, m in enumerate(lobby["members"]):
            if m["name"] == name:
                return i
        raise DemoError(f"member {name!r} not in lobby members: "
                        f"{[m['name'] for m in lobby['members']]}")

    def member_cid(self, lobby, name):
        return lobby["members"][self.member_index(lobby, name)]["cid"]

    def start_bot(self, name, join=None, create=False):
        b = Bot(self, name, join=join, create=create)
        self.bots.append(b)
        return b

    # ---------------------------------------------------- storyboard
    def reach_song_select(self):
        """Title -> main menu -> Rhythm Game -> song select (state 6)."""
        self.admin.tap("A")
        for _ in range(10):
            try:
                self.slog.wait(r"SEL '.*' INIT", 6, "pv_sel init")
                break
            except DemoError:
                # no INIT -> still in menus -> safe to keep pressing A
                self.admin.tap("A")
        else:
            raise DemoError("never reached song select (no SEL INIT)")
        # in pv_sel now - never tap blindly (an A would open the confirm
        # panel); just wait for the browse state
        self.slog.wait(r"SEL '.*' .*state=6", 60,
                       "song-select browse state")

    def step_browse(self):
        self.mark("Song select: browse songs, difficulties and filters")
        self.admin.tap("Down", count=6, hold=60, gap=640)
        self.hold(4.6)
        self.admin.tap("Up", count=2, hold=60, gap=640)
        self.hold(1.8)
        self.admin.tap("Right", count=2, hold=60, gap=640)
        self.hold(1.8)
        self.admin.tap("Left", count=2, hold=60, gap=640)
        self.hold(1.8)
        self.admin.tap("R", count=2, hold=60, gap=640)
        self.hold(1.8)
        self.admin.tap("L", count=2, hold=60, gap=640)
        self.hold(2.0)

    def step_open_overlay(self):
        self.mark("The Multiplayer button replaces the old AutoPlay badge")
        self.click(*MP_BADGE)
        self.hold(1.6)

    def step_home(self):
        self.mark("Multiplayer home")
        for c in (CARD_EDIT_NAME, CARD_CREATE, CARD_JOIN):
            self.glide(*c, dur=0.5)
            self.hold(0.7)
        self.hold(1.0)

    def step_edit_name(self):
        self.mark("Edit Name with the on-screen keyboard")
        for attempt in range(2):
            self.click(*CARD_EDIT_NAME)
            self.hold(1.0)                      # keyboard animates in
            self.click(*KB_CLR, glide=0.15)
            self.click(*KB_CAPS, glide=0.15)    # CAPS is a toggle
            self.type_text("m")
            self.click(*KB_CAPS, glide=0.15)
            self.type_text("iku")
            self.click(*DLG_OK, glide=0.2)      # SAVE
            # the game rewrites config.toml on set_name - verify there
            deadline = time.time() + 6
            saved = False
            while time.time() < deadline:
                try:
                    with open(CONFIG_TOML, errors="replace") as f:
                        if 'player_name = "Miku"' in f.read():
                            saved = True
                            break
                except OSError:
                    pass
                time.sleep(0.5)
            if saved:
                log("name saved (config.toml shows Miku)")
                return
            warn("name save not confirmed"
                 + (" - retrying" if attempt == 0 else " - continuing anyway"))
        self.hold(0.8)

    def step_join_room(self, code):
        self.mark("Join a room with its 4-letter code")
        for attempt in range(2):
            self.click(*CARD_JOIN)
            self.hold(1.0)
            if attempt:
                self.click(*KB_CLR, glide=0.15)  # drop partial code
            self.type_text(code)
            self.click(*DLG_OK, glide=0.2)      # JOIN
            try:
                self.slog.wait(rf"LOBBY lobby {code}: {self.client_cid} "
                               r"joined", 15, f"join lobby {code}")
                break
            except DemoError:
                if attempt:
                    raise
                warn("join click missed - retrying")
        self.mark("Room view as a member")
        self.hold(3.0)
        self.click(*ROOM_EXIT)
        self.wait_no_lobby(15)
        self.hold(0.8)

    def step_create_room(self):
        self.mark("Create a room - friends join live")
        code = None
        for attempt in range(2):
            self.click(*CARD_CREATE)
            try:
                m = self.slog.wait(rf"LOBBY lobby ([A-Z]+) created by "
                                   rf"{self.client_cid}", 15,
                                   "lobby created")
                code = m.group(1)
                break
            except DemoError:
                if attempt:
                    raise
                warn("create click missed - retrying")
        log(f"room {code} created")
        # bots join ~1.5s apart; Rin must leave her own lobby first
        rin = self.bots[0]
        rin.send("leave")
        self.hold(0.8)
        rin.send(f"join {code}")
        for name in BOT_NAMES[1:]:
            self.hold(1.5)
            self.start_bot(name, join=code)
        self.wait_members(6, 40)
        self.hold(1.5)

    def step_kick_neru(self):
        self.mark("Owners can remove players")
        _, l = self.wait_lobby(10)
        idx = self.member_index(l, "Neru")
        for attempt in range(2):
            self.click(KICK_X, KICK_Y0 + KICK_DY * idx)
            try:
                self.slog.wait(r"kicked by owner", 12, "Neru kicked")
                self.hold(1.5)
                return
            except DemoError:
                if attempt:
                    raise
                warn("kick click missed - retrying")

    def step_room_config(self):
        self.mark("Room Config: choose who picks the songs")
        self.click(*ROOM_CONFIG)
        self.hold(1.0)
        self.click(*RC_ROTATE)
        self.hold(0.9)
        _, l = self.wait_lobby(5)
        idx = self.member_index(l, "Luka")
        luka = self.member_cid(l, "Luka")
        # ASSIGNED + member cell together (re-click ASSIGNED on retry in case
        # the first one missed - a repeat click is a no-op once selected)
        for attempt in range(3):
            self.click(*RC_ASSIGNED)
            self.hold(0.8)
            self.click(GRID_X[idx % 2], GRID_Y0 + GRID_DY * (idx // 2))
            try:
                self.wait_leader(luka, 8)
                break
            except DemoError:
                if attempt == 2:
                    raise
                warn("ASSIGNED/member click missed - retrying")
        log("Luka assigned as picker")
        self.click(*RC_OWNER)
        self.wait_leader(self.client_cid, 10)
        self.click(*RC_DONE)
        self.hold(1.0)

    def step_minimised(self):
        self.mark("Closed: the button shows the room and who picks")
        self.click(*ROOM_CLOSE_X)
        self.hold(4.0)

    def step_autoplay(self):
        self.mark("AutoPlay (ZR x5): allowed in rooms and shown to everyone")
        self.admin.tap("ZR", count=5, hold=60, gap=170)
        # server rebroadcasts the member list with the autoplay flag;
        # bots print it as [auto]
        self.bots[0].wait(r"\[auto\]", 15,
                          "autoplay=on broadcast (member list)")
        log("autoplay=on confirmed via member broadcast")
        self.hold(1.5)

    def pick_song(self):
        """choose a demo pv the install actually has (via list_pvs)."""
        self.admin.raw({"cmd": "list_pvs"})
        deadline = time.time() + 12
        ids, total = set(), None
        while time.time() < deadline:
            self.slog._read()
            for m in re.finditer(
                    r"PVS '.*' i=\d+ n=(\d+) ids=\[([0-9, ]*)\]",
                    self.slog.buf):
                total = int(m.group(1))
                ids.update(int(x) for x in m.group(2).split(","))
            if total and len(ids) >= total:
                break
            time.sleep(0.3)
        for pv in [self.song] + [c for c in SONG_CANDIDATES
                                 if c != self.song]:
            if pv in ids:
                self.song = pv
                log(f"demo song pv={pv}")
                return
        warn(f"song list incomplete ({len(ids)}/{total} ids) - "
             f"using pv={self.song}")

    def step_pick_and_start(self):
        self.mark("The owner picks a song - every console follows")
        self.pick_song()
        self.admin.jump(self.song)
        self.slog.wait(r"JUMP '.*' .*'ok': True", 12, "jump landed")
        self.admin.tap("A")
        try:
            self.slog.wait(r"SEL '.*' .*state=8", 10, "confirm panel")
        except DemoError:
            self.admin.tap("A")
            self.slog.wait(r"SEL '.*' .*state=8", 10,
                           "confirm panel (retry)")
        self.hold(1.0)                          # panel ignores input ~0.5s
        self.admin.tap("A")
        try:
            self.slog.wait(r"LOBBY lobby \S+: launch r\d+", 15, "launch")
        except DemoError:
            self.admin.tap("A")                 # retry the commit
            self.slog.wait(r"LOBBY lobby \S+: launch r\d+", 15,
                           "launch (retry)")

    def step_loading_gate(self):
        self.mark("Waiting for every player to load")
        self.slog.wait(r"LOBBY lobby \S+: start r\d+", 90, "round start")

    def step_song(self):
        self.mark("Live leaderboard vs. bots")
        # verify AutoPlay is really scoring (MP Live board)
        self.slog.wait(r"DBG '.*' .*score=[1-9]", 25,
                       "client score rising (AutoPlay)")
        log("client score is rising - AutoPlay is playing")
        self.slog.wait(r"LOBBY lobby \S+: r\d+ standings complete", 300,
                       "round standings complete")

    def _wait_sel_or_retry(self, timeout):
        """watch the server log: 'sel' on SEL INIT, 'retry' if the client's
        dbg_warn fired (we hit RETRY and the song reloaded), 'timeout'."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.slog._read()
            m1 = re.search(r"SEL '.*' INIT", self.slog.buf)
            m2 = re.search(r"MSG '.*' .*dbg_warn", self.slog.buf)
            if m1 and (not m2 or m1.start() < m2.start()):
                self.slog.buf = self.slog.buf[m1.end():]
                return "sel"
            if m2:
                self.slog.buf = self.slog.buf[m2.end():]
                return "retry"
            time.sleep(0.2)
        return "timeout"

    def step_results(self):
        self.mark("Round standings")
        self.hold(4.0)                          # take in results + standings
        for attempt in range(3):
            # results -> song list recipe (round-5): A, A, Down, A
            self.admin.tap("A")
            self.hold(1.6)
            self.admin.tap("A")
            self.hold(1.6)
            self.admin.tap("Down")
            self.hold(0.6)
            self.admin.tap("A")
            outcome = self._wait_sel_or_retry(14)
            if outcome == "sel":
                break
            if outcome == "retry":
                warn("RETRY was hit - song reloaded; waiting for the next "
                     "results screen")
                self.slog.wait(r"RESULT '.*' ev=init", 240,
                               "second results screen")
                self.hold(3.0)
                continue
            warn(f"song-list recipe attempt {attempt + 1} gave no result")
        else:
            raise DemoError("never made it back to song select")
        self.slog.wait(r"LOBBY lobby \S+: (owner back_in_sel -> round_end|"
                       r"r\d+ end ->)", 30, "round_end")
        self.slog.wait(r"SEL '.*' .*state=6", 15,
                       "song-select browse state")

    def step_back_in_room(self):
        self.mark("Back in the room, ready for the next round")
        self.admin.overlay(True)
        self.hold(4.0)
        self.admin.overlay(False)
        self.hold(1.2)

    # ---------------------------------------------------- run phases
    def boot_to_sel(self):
        """wait for HELLO + fixed boot time, start capture, reach state 6."""
        self.wait_client_hello()
        left = self.ryu_launch_at + self.boot_wait - time.time()
        if left > 0:
            log(f"boot wait {left:.0f}s (post-HELLO)")
            time.sleep(left)
        self.rec = Recorder(self)
        self.rec.start()
        self.mark("Project DIVA Mega Mix on Switch, running the mod in "
                  "Ryujinx")
        self.reach_song_select()

    def tour(self):
        self.step_browse()
        self.step_open_overlay()
        self.step_home()
        self.step_edit_name()
        rin = self.start_bot("Rin", create=True)
        code = rin.wait(r"LOBBY_CODE ([A-Z]+)", 15, "Rin's lobby code")
        code = code.group(1)
        log(f"Rin's room: {code}")
        self.step_join_room(code)
        self.step_create_room()
        self.step_kick_neru()
        self.step_room_config()
        self.step_minimised()

    def round_demo(self):
        self.step_autoplay()
        self.step_pick_and_start()
        self.step_loading_gate()
        self.step_song()
        self.step_results()
        self.step_back_in_room()

    def skip_tour_setup(self):
        """create the room + join all bots via dbg_cmd (fast dev path)."""
        self.mark("Create a room - friends join live")
        self.admin.raw({"cmd": "lobby_create"})
        m = self.slog.wait(rf"LOBBY lobby ([A-Z]+) created by "
                           rf"{self.client_cid}", 15, "lobby created")
        code = m.group(1)
        self.start_bot("Rin", join=code)
        for name in BOT_NAMES[1:]:
            self.hold(0.8)
            self.start_bot(name, join=code)
        self.wait_members(6, 40)
        self.hold(1.0)

    # ---------------------------------------------------- post process
    def srt_time(self, t):
        ms = max(0, int(round(t * 1000)))
        h, ms = divmod(ms, 3600000)
        m, ms = divmod(ms, 60000)
        s, ms = divmod(ms, 1000)
        return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"

    def write_srt(self, path):
        lines = []
        for i, (t, txt) in enumerate(self.captions):
            t1 = (self.captions[i + 1][0] if i + 1 < len(self.captions)
                  else self.video_end)
            lines.append(f"{i+1}\n{self.srt_time(t)} --> "
                         f"{self.srt_time(t1)}\n{txt}\n")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def write_ffmeta(self, path):
        out = [";FFMETADATA1"]
        for i, (t, txt) in enumerate(self.captions):
            t1 = (self.captions[i + 1][0] if i + 1 < len(self.captions)
                  else self.video_end)
            out += ["[CHAPTER]", "TIMEBASE=1/1000",
                    f"START={int(t*1000)}", f"END={int(t1*1000)}",
                    f"title={txt}"]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")

    def audio_mean_db(self, raw):
        r = subprocess.run(
            [self.ffmpeg, "-hide_banner", "-i", raw, "-map", "0:a:0?",
             "-af", "highpass=f=200,volumedetect", "-f", "null", "-"],
            capture_output=True, text=True, timeout=180)
        m = re.search(r"mean_volume:\s*(-?[\d.]+)\s*dB", r.stderr or "")
        return float(m.group(1)) if m else None

    def postprocess(self):
        raw = os.path.join(self.workdir, "raw.mkv")
        if not os.path.exists(raw) or os.path.getsize(raw) < 100_000:
            warn("raw.mkv missing/tiny - skipping postprocess")
            return
        if self.rec and self.rec.rec_t0:
            end = self.rec.stopped_at or time.time()
            self.video_end = end - self.rec.rec_t0
        else:
            self.video_end = (self.captions[-1][0] + 5) if self.captions \
                else 0.0
        if self.captions:
            self.video_end = max(self.video_end, self.captions[-1][0] + 1.5)
        srt = os.path.join(self.workdir, "captions.srt")
        self.write_srt(srt)
        ffmeta = os.path.join(self.workdir, "chapters.ffmeta")
        self.write_ffmeta(ffmeta)

        keep_audio = False
        if self.rec and self.rec.with_audio:
            mean = self.audio_mean_db(raw)
            log(f"raw audio mean_volume (highpass 200Hz): "
                f"{mean if mean is not None else 'n/a'} dB")
            if mean is not None and mean >= -40.0:
                keep_audio = True
            else:
                warn("recorded audio is effectively silent "
                     "(mean < -40 dB) - MP4 will have no audio track")

        os.makedirs(self.out_dir, exist_ok=True)
        stem = "megamix-mod-demo_" + datetime.now().strftime("%Y%m%d-%H%M")
        self.out_mp4 = os.path.join(self.out_dir, stem + ".mp4")
        style = ("FontSize=11,PrimaryColour=&H00FFFFFF,"
                 "BorderStyle=3,BackColour=&H78000000,"
                 "Outline=2,Shadow=0,Alignment=2,MarginV=12")
        argv = [self.ffmpeg, "-hide_banner", "-y",
                "-i", "raw.mkv", "-i", "chapters.ffmeta",
                "-map", "0:v"]
        if keep_audio:
            argv += ["-map", "0:a"]
        # NOTE: -map_metadata 1 would wipe chapter titles - don't add it
        argv += ["-map_chapters", "1",
                 "-vf", f"subtitles=captions.srt:force_style='{style}'"]
        argv += self.enc_args
        if keep_audio:
            argv += ["-c:a", "aac", "-b:a", "192k"]
        argv += ["-movflags", "+faststart", self.out_mp4]
        r = subprocess.run(argv, cwd=self.workdir, capture_output=True,
                           text=True, timeout=900)
        with open(os.path.join(self.workdir, "ffmpeg_post.log"), "w") as f:
            f.write("$ " + " ".join(argv) + "\n\n" + (r.stderr or ""))
        if r.returncode != 0 or not os.path.exists(self.out_mp4):
            raise DemoError("final encode failed - see ffmpeg_post.log\n"
                            + (r.stderr or "")[-1200:])
        shutil.copy2(srt, os.path.join(self.out_dir, stem + ".srt"))
        log(f"wrote {self.out_mp4}")
        if not self.keep_raw:
            try:
                os.remove(raw)
            except OSError:
                pass
        self.report()

    def report(self):
        d = subprocess.run(
            [self.ffprobe, "-v", "error", "-show_entries",
             "format=duration,size:stream=codec_type,codec_name",
             "-of", "json", self.out_mp4],
            capture_output=True, text=True, timeout=30)
        info = json.loads(d.stdout or "{}")
        streams = [f"{s.get('codec_type')}:{s.get('codec_name')}"
                   for s in info.get("streams", [])]
        fmt = info.get("format", {})
        log(f"MP4 {self.out_mp4}  {float(fmt.get('duration', 0)):.1f}s  "
            f"{int(fmt.get('size', 0))/1e6:.1f} MB  streams={streams}")
        log("chapters:")
        for t, c in self.captions:
            log(f"  {self.srt_time(t).replace(',', '.')}  {c}")
        if self.frames_dir:
            self.extract_frames()

    def extract_frames(self):
        os.makedirs(self.frames_dir, exist_ok=True)
        for i, (t, txt) in enumerate(self.captions):
            t1 = (self.captions[i + 1][0] if i + 1 < len(self.captions)
                  else self.video_end)
            mid = (t + t1) / 2
            out = os.path.join(self.frames_dir, f"ch{i+1:02d}.png")
            subprocess.run([self.ffmpeg, "-hide_banner", "-y", "-ss",
                            f"{mid:.2f}", "-i", self.out_mp4,
                            "-frames:v", "1", "-vf", "scale=960:-1", out],
                           capture_output=True, timeout=60)
            log(f"frame ch{i+1:02d} @ {mid:.1f}s -> {out}")

    # ---------------------------------------------------- main
    def run(self):
        os.makedirs(self.workdir, exist_ok=True)
        log(f"workdir: {self.workdir}")
        self.preflight()
        self.kill_ryujinx()
        self.patch_config()
        try:
            self.start_server()
            self.launch_ryujinx()
            self.boot_to_sel()
            if self.skip_tour:
                self.skip_tour_setup()
            else:
                self.tour()
            self.round_demo()
            log("storyboard finished - stopping capture")
        finally:
            if self.rec:
                self.rec.stop()
            self.kill_tracked()
            self.restore_config()
        self.postprocess()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port-base", type=int, default=37777,
                    help="server TCP port; discovery=+1, admin=+13 "
                         "(default 37777/37778/37790)")
    ap.add_argument("--ryujinx", default=RYUJINX_EXE)
    ap.add_argument("--nsp", default=NSP)
    ap.add_argument("--monitor", type=int, default=0,
                    help="ddagrab output_idx (fullscreen monitor)")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--audio-device", default=AUDIO_DEV)
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--skip-tour", action="store_true",
                    help="skip the UI tour: create room + bots, go straight "
                         "to the multiplayer round")
    ap.add_argument("--keep-raw", action="store_true")
    ap.add_argument("--song", type=int, default=700,
                    help="demo song pv id (default 700 = Ievan Polkka)")
    ap.add_argument("--song-secs", type=float, default=120,
                    help="bot song length, a bit shorter than the real song")
    ap.add_argument("--bot-load-delay", type=float, default=3.0)
    ap.add_argument("--boot-wait", type=float, default=58,
                    help="seconds after Ryujinx launch before recording "
                         "starts")
    ap.add_argument("--out-dir", default=OUT_DIR)
    ap.add_argument("--work-dir", default=None)
    ap.add_argument("--frames-dir", default=None,
                    help="if set, extract a 960px PNG at each chapter's "
                         "midpoint into this dir")
    a = ap.parse_args()

    demo = Demo(a)
    try:
        demo.run()
    except DemoError as e:
        warn(f"FAILED: {e}")
        if demo.out_mp4 is None:
            try:
                demo.postprocess()
            except Exception:
                pass
        sys.exit(1)
    except KeyboardInterrupt:
        warn("interrupted")
        sys.exit(130)


if __name__ == "__main__":
    main()
