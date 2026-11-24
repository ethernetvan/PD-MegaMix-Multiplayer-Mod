#!/usr/bin/env python3
"""package_emu_pack.py - build the client mod and pack a ready-to-play
Ryujinx + Mega Mix + mods bundle for another Windows PC.

Produces (in --out, default pack_out/):

  MegaMix_Pack_Setup.zip         Ryujinx binaries + RyuData (keys, firmware,
                                 saves, config, mod exefs), Play/Combine
                                 bats, README, optional server copy
  MegaMix_Pack_SwitchMods[ _partNNN].zip   romfs mods/config pulled from the
                                 real Switch (skipped when unreachable)
  MegaMix_Pack_Game_partNNN.zip  the NSP split into --part-mb chunks
                                 (default 4000 MB so every zip fits a FAT32
                                 USB stick); the recipient double-clicks
                                 "Combine Game Files.bat" to rebuild the .nsp

The staged pack gets a fresh random multiplayer client_id and player_name
so it never collides with this machine's identity on the server.

Switch-side content (needs sys-ftpd-light on the console, NX_IP or --nx-ip):
  /MikuMikuSwitchPlugin/                    -> RyuData/sdcard/... (config+save)
  /atmosphere/contents/<TID>/romfs/mods/*   -> plugin mod path on the emu sdcard
  /atmosphere/contents/<TID>/romfs/* (rest) -> Ryujinx layeredfs mod dir
  /atmosphere/contents/<TID>/exefs_patches  -> Ryujinx mod dir (ips patches)
  /atmosphere/contents/<TID>/cheats         -> Ryujinx cheats dir

Usage:
  python tools/package_emu_pack.py                 # full: build + pull + zip
  python tools/package_emu_pack.py --skip-build    # reuse installed/built exefs
  python tools/package_emu_pack.py --skip-switch   # no console pull
  python tools/package_emu_pack.py --skip-zip      # stage only (debug)
"""
import argparse
import ftplib
import json
import os
import posixpath
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

# ------------------------------------------------------------------ constants
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCM = os.path.join(REPO, "switch-client-mod")
DEPLOY = os.path.join(SCM, "deploy")
TID = "01001CC00FA1A000"
TID_DIR = TID.lower()

RYU_BIN = r"C:\Users\cl672\Documents\Juegos\Ryujinx"
NSP = r"C:\Users\cl672\Documents\Juegos\switchgames\Hatsune_Miku_Merged.nsp"
RYU_DATA = os.path.join(os.environ.get("APPDATA", ""), "Ryujinx")
DEFAULT_NX_IP = os.environ.get("NX_IP", "10.167.0.198")

PACK_ROOT_NAME = "MegaMixPack"          # top-level dir inside every zip
# pulled Switch trees -> where they land inside the pack (below MegaMixPack/)
SWITCH_STAGED = [
    ("sd_atmo_romfs", f"RyuData/sdcard/atmosphere/contents/{TID}/romfs"),
    ("ryu_romfs",     f"RyuData/mods/contents/{TID}/switch_romfs_layer/romfs"),
    ("exefs_patches", f"RyuData/mods/contents/{TID}/switch_exefs_patches/exefs_patches"),
    ("cheats",        f"RyuData/mods/contents/{TID}/cheats"),
]
DATA_DIRS = ["system", "bis", "mods", "profiles"]
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
NM_BAD = r"Config::|mp::|FontLoader::|AutoPlay::|Hints::"


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def warn(msg):
    print(f"[{time.strftime('%H:%M:%S')}] !! {msg}", flush=True)


def die(msg):
    warn(msg)
    sys.exit(1)


# ------------------------------------------------------------------ build
def find_bash():
    """Bare `bash` may resolve to WSL's bash (broken here). devkitPro's msys2
    bash is the one that sees /opt/devkitpro - probe candidates for it."""
    cands = []
    dp = os.environ.get("DEVKITPRO")
    if dp:
        cands.append(os.path.join(dp, "msys2", "usr", "bin", "bash.exe"))
    cands += [r"C:\devkitPro\msys2\usr\bin\bash.exe",
              r"C:\Program Files\Git\usr\bin\bash.exe",
              r"C:\Program Files\Git\bin\bash.exe"]
    w = shutil.which("bash")
    if w:
        cands.append(w)
    for b in cands:
        if not os.path.exists(b):
            continue
        r = subprocess.run([b, "-c", f"test -x {NM}"], capture_output=True)
        if r.returncode == 0:
            return b
    die("no bash with /opt/devkitpro found - install devkitPro "
        "(expected at C:\\devkitPro)")


def build_mod():
    bash = find_bash()
    log(f"building switch-client-mod (exlaunch.sh build via {bash}) ...")
    r = subprocess.run([bash, "exlaunch.sh", "build"], cwd=SCM)
    if r.returncode != 0:
        die("build failed - fix the build before packaging")
    # a ' U ' MP symbol = an edit never hit the disk -> crash at boot
    r = subprocess.run(
        [bash, "-c",
         f"{NM} -C switch-client-mod.elf | grep ' U ' | grep -E '{NM_BAD}'"],
        cwd=SCM, capture_output=True, text=True)
    if r.stdout.strip():
        die("unresolved MP symbols in the elf - edits did not reach disk:\n"
            + r.stdout)
    for f in ("subsdk9", "main.npdm"):
        if not os.path.exists(os.path.join(DEPLOY, f)):
            die(f"build produced no deploy/{f}")
    log("build ok, deploy/ has subsdk9 + main.npdm")


def git_rev():
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           cwd=REPO, capture_output=True, text=True)
        return r.stdout.strip() or "unknown"
    except OSError:
        return "unknown"


# ------------------------------------------------------------------ switch FTP
class SwitchFTP:
    """Recursive pull from sys-ftpd-light. Uses MLSD (one call per dir:
    name + type + size, no per-entry cwd probing). sys-ftpd is fragile
    under many rapid transfers: the control connection can die mid-pull,
    so every op reconnects and retries."""

    def __init__(self, ip):
        self.ip = ip
        self.f = ftplib.FTP()
        self.f.connect(ip, 5000, timeout=15)
        self.f.login()
        self._rx = self._rx_mark = 0
        self.failed = []

    def close(self):
        try:
            self.f.quit()
        except Exception:
            pass

    def reconnect(self):
        try:
            self.f.quit()
        except Exception:
            pass
        time.sleep(2)
        self.f = ftplib.FTP()
        self.f.connect(self.ip, 5000, timeout=15)
        self.f.login()
        log("    (FTP reconnected)")

    def ls(self, path, retry=2):
        """List a dir -> [(abs_path, name, is_dir, size)]."""
        out = []
        try:
            for name, facts in self.f.mlsd(path):
                if name in (".", "..") or facts.get("type") == "cdir":
                    continue
                out.append((posixpath.join(path, name), name,
                            facts.get("type") == "dir",
                            int(facts.get("size", 0))))
        except ftplib.error_perm:
            pass
        except (ftplib.Error, OSError) as e:
            if retry:
                warn(f"  FTP ls failed for {path} ({e}) - reconnecting")
                self.reconnect()
                return self.ls(path, retry - 1)
            warn(f"  FTP ls gave up on {path} - subtree skipped")
            self.failed.append(path + "/")
        return out

    def pull_file(self, remote, local, retry=2):
        try:
            os.makedirs(os.path.dirname(local), exist_ok=True)
            with open(local, "wb") as fh:
                def cb(d):
                    fh.write(d)
                    self._rx += len(d)
                    if self._rx - self._rx_mark >= 256_000_000:
                        self._rx_mark = self._rx
                        log(f"    ... {self._rx/1e9:.1f} GB pulled")
                self.f.retrbinary("RETR " + remote, cb)
            time.sleep(0.02)      # be gentle with sys-ftpd
        except (ftplib.Error, OSError) as e:
            if retry:
                warn(f"  FTP pull dropped on {remote} ({e}) - retrying")
                self.reconnect()
                return self.pull_file(remote, local, retry - 1)
            if os.path.exists(local):
                os.unlink(local)   # remove the truncated file
            self.failed.append(remote)
            raise

    def pull_tree(self, remote, local_root):
        """Recursively download remote dir into local_root. -> (files, bytes)"""
        n = sz = 0
        for entry, name, isdir, size in self.ls(remote):
            if isdir:
                f2, s2 = self.pull_tree(
                    entry, os.path.join(local_root, name))
                n += f2
                sz += s2
            else:
                dst = os.path.join(local_root, name)
                try:
                    self.pull_file(entry, dst)
                    n += 1
                    sz += os.path.getsize(dst)
                except (ftplib.Error, OSError) as e:
                    warn(f"  FTP pull failed for {entry}: {e}")
        return n, sz


def pull_switch_content(ip, tmpdir):
    """Pull everything worth having from the real Switch SD into tmpdir.
    Returns a dict of pulled trees or None if unreachable."""
    log(f"contacting Switch at {ip}:5000 (sys-ftpd-light) ...")
    try:
        sw = SwitchFTP(ip)
    except (OSError, ftplib.Error) as e:
        warn(f"Switch not reachable ({e}) - packing WITHOUT Switch mods")
        return None
    out = {}

    # 1. plugin config + save (authoritative copy - real priority ordering)
    n, sz = sw.pull_tree("/MikuMikuSwitchPlugin",
                         os.path.join(tmpdir, "MikuMikuSwitchPlugin"))
    if n:
        out["plugin"] = os.path.join(tmpdir, "MikuMikuSwitchPlugin")
        log(f"  /MikuMikuSwitchPlugin: {n} files, {sz/1e6:.1f} MB")

    # 2. atmosphere contents dir for our TID
    base = f"/atmosphere/contents/{TID}"
    entries = sw.ls(base)
    if not entries:
        warn(f"  {base} not found on the Switch SD - no atmosphere mods?")
    for entry, name, isdir, _size in entries:
        if not isdir:
            log(f"  skip stray file {entry}")
            continue
        if name == "romfs":
            # plugin mods live under romfs/mods/<name>; anything else directly
            # under romfs/ is a plain atmosphere overlay -> Ryujinx layeredfs
            for sub, sname, sdir, ssize in sw.ls(entry):
                if sname == "mods" and sdir:
                    n, sz = sw.pull_tree(
                        sub, os.path.join(tmpdir, "sd_atmo_romfs", "mods"))
                    log(f"  romfs/mods: {n} files, {sz/1e6:.1f} MB -> "
                        "plugin mods on emu sdcard")
                    if n:
                        out["sd_atmo_romfs"] = os.path.join(
                            tmpdir, "sd_atmo_romfs")
                elif sdir:
                    n, sz = sw.pull_tree(
                        sub, os.path.join(tmpdir, "ryu_romfs", sname))
                    log(f"  romfs/{sname}: {n} files, {sz/1e6:.1f} MB -> "
                        "Ryujinx layeredfs mod")
                    if n:
                        out["ryu_romfs"] = os.path.join(tmpdir, "ryu_romfs")
                else:
                    try:
                        sw.pull_file(sub, os.path.join(tmpdir, "ryu_romfs",
                                                       sname))
                        log(f"  romfs/{sname} -> Ryujinx layeredfs mod")
                        out["ryu_romfs"] = os.path.join(tmpdir, "ryu_romfs")
                    except (ftplib.Error, OSError) as e:
                        warn(f"  FTP pull failed for {sub}: {e}")
        elif name == "exefs_patches":
            n, sz = sw.pull_tree(entry, os.path.join(tmpdir, "exefs_patches"))
            log(f"  exefs_patches: {n} files, {sz/1e6:.1f} MB")
            if n:
                out["exefs_patches"] = os.path.join(tmpdir, "exefs_patches")
        elif name == "cheats":
            n, sz = sw.pull_tree(entry, os.path.join(tmpdir, "cheats"))
            log(f"  cheats: {n} files, {sz/1e6:.1f} MB")
            if n:
                out["cheats"] = os.path.join(tmpdir, "cheats")
        elif name in ("exefs", "flags"):
            log(f"  skip {name} (fresh build replaces exefs)")
        else:
            warn(f"  unknown dir {entry} - skipped")
    if sw.failed:
        warn(f"{len(sw.failed)} remote path(s) failed to pull:")
        for p in sw.failed[:20]:
            warn(f"    {p}")
        warn("re-run the script to retry - do NOT ship a partial pack")
    sw.close()
    return out


# ------------------------------------------------------------------ staging
def copytree(src, dst, ignore=None):
    if os.path.exists(src):
        shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


def stage(pack, a, cid, name, pulled):
    root = os.path.join(pack, PACK_ROOT_NAME)
    data = os.path.join(root, "RyuData")
    os.makedirs(data, exist_ok=True)
    log(f"staging {root}")

    # Ryujinx binaries (no Logs)
    copytree(RYU_BIN, os.path.join(root, "Ryujinx"),
             ignore=shutil.ignore_patterns("Logs", "portable"))

    # data root: keys, firmware, saves, emulator mods dir, cache (optional)
    for d in DATA_DIRS:
        copytree(os.path.join(RYU_DATA, d), os.path.join(data, d))
    if not a.no_cache:
        copytree(os.path.join(RYU_DATA, "games"), os.path.join(data, "games"))
    else:
        log("  skipping games/ cache (--no-cache)")

    # sdcard: only the plugin dir (local copy); switch pull may replace it
    copytree(os.path.join(RYU_DATA, "sdcard", "MikuMikuSwitchPlugin"),
             os.path.join(data, "sdcard", "MikuMikuSwitchPlugin"))

    # emulator settings, patched for a fresh machine
    cfg = json.load(open(os.path.join(RYU_DATA, "Config.json"),
                         encoding="utf-8"))
    cfg["enable_internet_access"] = True          # required for MP sockets
    cfg["game_dirs"] = []
    cfg["autoload_dirs"] = []
    cfg["preferred_gpu"] = ""
    cfg["enable_discord_integration"] = False
    cfg["start_fullscreen"] = False
    cfg["window_startup"] = {"window_size_width": 1280,
                             "window_size_height": 760,
                             "window_position_x": 100,
                             "window_position_y": 100,
                             "window_maximized": False}
    json.dump(cfg, open(os.path.join(data, "Config.json"), "w"), indent=2)

    # fresh mod build (or the currently-installed exefs with --skip-build)
    exefs = os.path.join(data, "mods", "contents", TID, "mods", "exefs")
    shutil.rmtree(os.path.dirname(exefs), ignore_errors=True)
    os.makedirs(exefs, exist_ok=True)
    src = DEPLOY if os.path.exists(os.path.join(DEPLOY, "subsdk9")) else \
        os.path.join(RYU_DATA, "mods", "contents", TID, "mods", "exefs")
    copytree(src, exefs)
    have = sorted(os.listdir(exefs))
    if have != ["main.npdm", "subsdk9"]:
        die(f"staged exefs must be exactly main.npdm + subsdk9, got {have}")
    log(f"  mod exefs <- {src}")

    # server copy (optional self-host on the other PC)
    srv = os.path.join(root, "server")
    os.makedirs(srv, exist_ok=True)
    for f in os.listdir(os.path.join(REPO, "server")):
        if f.endswith(".py"):
            shutil.copy2(os.path.join(REPO, "server", f), srv)

    # Switch content (move = rename, no re-copy of the pulled GBs)
    if pulled:
        if pulled.get("plugin"):
            dst = os.path.join(data, "sdcard", "MikuMikuSwitchPlugin")
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(pulled["plugin"], dst)
            log("  sdcard MikuMikuSwitchPlugin <- Switch copy")
        for key, rel in SWITCH_STAGED:
            if pulled.get(key):
                dst = os.path.join(root, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.exists(dst):
                    shutil.rmtree(dst)
                shutil.move(pulled[key], dst)
                log(f"  staged {rel}")

    # patch the winning config.toml: unique cid + name
    ctoml = os.path.join(data, "sdcard", "MikuMikuSwitchPlugin", "config.toml")
    patch_config_toml(ctoml, cid, name)

    # launchers + docs
    write_text(os.path.join(root, "Play Mega Mix.bat"), PLAY_BAT)
    os.makedirs(os.path.join(root, "Games"), exist_ok=True)
    nsp_size = os.path.getsize(NSP)
    write_text(os.path.join(root, "Games", "Combine Game Files.bat"),
               combine_bat(nsp_size))
    write_text(os.path.join(root, "server", "Run Server (optional).bat"),
               SERVER_BAT)
    write_text(os.path.join(root, "README - START HERE.txt"),
               readme(cid, name, nsp_size, git_rev(), pulled, a.skip_switch))
    return root


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8", newline="\r\n").write(text)


def patch_config_toml(path, cid, name):
    """Set client_id/player_name inside [multiplayer], keep everything else.
    Also fixes two pulled-config traps for a fresh device: server_mode "off"
    means it never connects -> force "local"; a stale server_ip only adds a
    dead connect attempt -> blank it (LAN discovery + mp_servers.txt carry
    the real history)."""
    wanted = {"enabled": "true", "player_name": f'"{name}"',
              "client_id": f'"{cid}"',
              # this PC's secret stays here (the pack's game makes its own);
              # the server's debug remote control stays off
              "client_key": '""', "debug_commands": "false"}
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        lines = ["[multiplayer]"]
    else:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    in_mp, mp_start, seen = False, -1, set()
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
        m = re.match(r"\s*(" + "|".join(wanted) + r")\s*=", ln)
        if m:
            key = m.group(1)
            cmt = re.search(r"#.*$", ln[m.end():])
            lines[i] = f"{key} = {wanted[key]}" + \
                ((" " + cmt.group(0)) if cmt else "")
            seen.add(key)
            continue
        if re.match(r'\s*server_mode\s*=\s*"off"', ln):
            lines[i] = 'server_mode = "local"'
            log("  config.toml: server_mode off -> local")
        elif re.match(r"\s*server_ip\s*=", ln):
            lines[i] = 'server_ip = ""'
            log("  config.toml: server_ip blanked (stale address)")
    if mp_start < 0:
        lines += ["", "[multiplayer]"] + \
            [f"{k} = {v}" for k, v in wanted.items()]
    else:
        missing = [f"{k} = {v}" for k, v in wanted.items() if k not in seen]
        lines[mp_start + 1:mp_start + 1] = missing
    open(path, "w", encoding="utf-8", newline="").write("\n".join(lines))
    log(f"  config.toml: player_name={name} client_id={cid}")


# ------------------------------------------------------------------ zip
def zip_dir(src_root, zip_path, exclude=()):
    """Zip the staged MegaMixPack tree (arcnames start with MegaMixPack/).
    `exclude` = pack-relative paths (e.g. MegaMixPack/RyuData/x) to skip."""
    excl = [e.replace(os.sep, "/").rstrip("/") + "/" for e in exclude]
    n = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=6, allowZip64=True) as z:
        for dirpath, dirs, files in os.walk(src_root):
            for d in dirs:                       # keep empty dirs too
                p = os.path.join(dirpath, d)
                if not os.listdir(p):
                    rel = os.path.relpath(
                        p, os.path.dirname(src_root)).replace(os.sep, "/")
                    if not any(rel.startswith(x) for x in excl):
                        z.write(p, rel + "/")
            for f in files:
                p = os.path.join(dirpath, f)
                rel = os.path.relpath(
                    p, os.path.dirname(src_root)).replace(os.sep, "/")
                if any(rel.startswith(x) for x in excl):
                    continue
                z.write(p, rel)
                n += 1
    log(f"  {os.path.basename(zip_path)}: {n} files, "
        f"{os.path.getsize(zip_path)/1e9:.2f} GB")


def zip_switch_mods(stage_root, staged_rels, outdir, cap_bytes):
    """Zip the pulled Switch trees into MegaMix_Pack_SwitchMods[ _partNNN].zip,
    splitting only at file boundaries (capped by raw size; deflate only shrinks).
    Returns list of created zips."""
    todo = []
    for rel in staged_rels:
        base = os.path.join(stage_root, PACK_ROOT_NAME, rel)
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                p = os.path.join(dirpath, f)
                rel_arc = os.path.relpath(p, stage_root).replace(os.sep, "/")
                todo.append((p, rel_arc, os.path.getsize(p)))
    if not todo:
        return []
    todo.sort(key=lambda t: t[1])
    zips, cur, cur_sz, part = [], None, 0, 0
    for p, arc, sz in todo:
        if cur is None or cur_sz + sz > cap_bytes:
            if cur is not None:
                cur.close()
            part += 1
            zp = os.path.join(
                outdir, f"MegaMix_Pack_SwitchMods_part{part:03d}.zip")
            cur = zipfile.ZipFile(zp, "w", zipfile.ZIP_DEFLATED,
                                  compresslevel=6, allowZip64=True)
            cur_path = zp
            zips.append(zp)
            cur_sz = 0
            if sz > cap_bytes:
                warn(f"{arc} alone is {sz/1e9:.2f} GB > part cap - "
                     "gets its own oversize zip")
        cur.write(p, arc)
        cur_sz += sz
    cur.close()
    if len(zips) == 1:
        nice = os.path.join(outdir, "MegaMix_Pack_SwitchMods.zip")
        os.replace(zips[0], nice)
        zips = [nice]
    for zp in zips:
        log(f"  {os.path.basename(zp)} ({os.path.getsize(zp)/1e9:.2f} GB)")
    return zips


def zip_game_parts(nsp, outdir, part_mb):
    """Split the NSP into chunks; each chunk -> its own zip."""
    part_size = int(part_mb * 1_000_000)
    total = os.path.getsize(nsp)
    nparts = (total + part_size - 1) // part_size
    if nparts == 1:
        # small enough for a single zip - store it as the final file name
        zp = os.path.join(outdir, "MegaMix_Pack_Game.zip")
        with zipfile.ZipFile(zp, "w", zipfile.ZIP_STORED,
                             allowZip64=True) as z:
            z.write(nsp, f"{PACK_ROOT_NAME}/Games/Hatsune_Miku_Merged.nsp")
        log(f"  {os.path.basename(zp)} ({os.path.getsize(zp)/1e9:.2f} GB)")
        return [zp]
    log(f"splitting {total/1e9:.2f} GB NSP into {nparts} part(s) "
        f"of <= {part_mb} MB")
    zips = []
    written = 0
    with open(nsp, "rb") as src:
        for i in range(1, nparts + 1):
            pname = f"Hatsune_Miku_Merged.nsp.part{i:03d}"
            arc = f"{PACK_ROOT_NAME}/Games/{pname}"
            zp = os.path.join(
                outdir, f"MegaMix_Pack_Game_part{i:03d}.zip")
            chunk = min(part_size, total - written)
            with tempfile.NamedTemporaryFile(
                    delete=False, dir=outdir, suffix=".part") as tmp:
                shutil.copyfileobj(src, tmp, chunk)
                tmp_path = tmp.name
            written += chunk
            with zipfile.ZipFile(zp, "w", zipfile.ZIP_STORED,
                                 allowZip64=True) as z:
                z.write(tmp_path, arc)
            os.unlink(tmp_path)
            zips.append(zp)
            log(f"  {os.path.basename(zp)} "
                f"({os.path.getsize(zp)/1e9:.2f} GB)")
    return zips


# ------------------------------------------------------------------ bat/readme
PLAY_BAT = r"""@echo off
rem Double-click this to play Mega Mix with the mod.
cd /d "%~dp0"
if not exist "%~dp0Games\Hatsune_Miku_Merged.nsp" (
    echo The game file is missing.
    echo Open the Games folder and double-click "Combine Game Files.bat" first.
    pause
    exit /b 1
)
start "Mega Mix" /d "%~dp0Ryujinx" "%~dp0Ryujinx\Ryujinx.exe" -r "%~dp0RyuData" "%~dp0Games\Hatsune_Miku_Merged.nsp"
"""

SERVER_BAT = r"""@echo off
rem OPTIONAL: runs the multiplayer lobby server on THIS computer instead.
rem Needs Python 3.10+ installed (https://www.python.org/downloads/).
cd /d "%~dp0"
python mp_server.py
pause
"""


def combine_bat(expected_size):
    return f"""@echo off
rem Rebuilds the game file from the .part pieces - run this ONCE.
cd /d "%~dp0"
if exist Hatsune_Miku_Merged.nsp (
    for %%I in (Hatsune_Miku_Merged.nsp) do if %%~zI=={expected_size} (
        echo The game file already exists - nothing to do.
        pause
        exit /b 0
    )
    echo Removing an incomplete game file from an earlier attempt...
    del Hatsune_Miku_Merged.nsp
)
echo Combining game parts, this can take a few minutes...
copy /b Hatsune_Miku_Merged.nsp.part* Hatsune_Miku_Merged.nsp >nul 2>&1
set SIZE=0
for %%I in (Hatsune_Miku_Merged.nsp) do set SIZE=%%~zI
if "%SIZE%"=="{expected_size}" (
    echo.
    echo DONE - the game file is complete.
    echo You can now delete every Hatsune_Miku_Merged.nsp.part* file
    echo in this folder to free up space.
) else (
    echo.
    echo WARNING - the combined file is %SIZE% bytes but should be {expected_size}.
    echo Some parts are missing. Extract EVERY MegaMix_Pack_*_Game_part*.zip
    echo into this same folder and run this file again.
)
pause
"""


def readme(cid, name, nsp_size, rev, pulled, skipped):
    if skipped:
        mods_line = ("NOTE: built with --skip-switch, so NO extra Switch "
                     "mods are included in this pack.")
    elif pulled:
        mods_line = ("The pack also includes the extra mods pulled from "
                     "your Switch SD card (they load automatically).")
    else:
        mods_line = ("NOTE: the Switch was not reachable, so NO extra "
                     "Switch mods are included in this pack.")
    return f"""==============================================================
 MEGA MIX + MULTIPLAYER MOD - EMULATOR PACK
==============================================================
A ready-to-play copy of Hatsune Miku: Project DIVA Mega Mix for a
Windows PC, with the multiplayer mod pre-installed.
{mods_line}

Your multiplayer name in this copy: {name}
(unique id {cid} - do not copy this folder over another install or
two players will share one identity)

Build: {rev} packed {time.strftime('%Y-%m-%d')}


--------------------------------------------------------------
WHAT YOU NEED
--------------------------------------------------------------
* A 64-bit Windows 10/11 computer
* About 30 GB of free disk space (you can shrink to ~16 GB later)
* ALL the MegaMix_Pack_*.zip files


--------------------------------------------------------------
STEP 1 - UNPACK EVERYTHING INTO ONE FOLDER
--------------------------------------------------------------
1. Make a new folder, e.g.  Documents\\MegaMixPack
2. Extract EVERY MegaMix_Pack_*.zip into that same folder
   (right-click -> "Extract All..." -> choose your folder).
   The zips are parts of one set - they all belong together.
3. When finished, the folder must contain:
     Ryujinx\\   RyuData\\   Games\\   server\\   Play Mega Mix.bat


--------------------------------------------------------------
STEP 2 - BUILD THE GAME FILE (once)
--------------------------------------------------------------
The 15 GB game file was split into smaller pieces for copying.
1. Open the  Games  folder.
2. Double-click  "Combine Game Files.bat".
3. Wait until it says DONE. If it warns about missing parts, one of
   the Game part zips was not extracted yet - extract it and re-run.
4. Afterwards you can delete every  Hatsune_Miku_Merged.nsp.part*
   file in Games\\ to free about 15 GB.


--------------------------------------------------------------
STEP 3 - PLAY
--------------------------------------------------------------
Double-click  "Play Mega Mix.bat".

* First boot can take a minute or two.
* If Windows Firewall asks about Ryujinx network access, click
  "Allow" - multiplayer needs it.
* No controller? In the Ryujinx window: Options -> Settings -> Input,
  set Player 1 to your keyboard or gamepad, then Save.
  (In-game keyboard play is also possible: edit
  RyuData\\sdcard\\MikuMikuSwitchPlugin\\config.toml and set
  enable_keyboard = true under [gameplay].)


--------------------------------------------------------------
MULTIPLAYER
--------------------------------------------------------------
* On the same Wi-Fi as the computer running the lobby server, the game
  finds it automatically - nothing to configure. The host computer runs:
      python server\\mp_server.py
  (there is a copy in the server\\ folder plus "Run Server (optional).bat"
  if this PC should be the host - it needs Python installed).
* On a different network, ask the host which to use:
    - set server_ip = "<their IP>"  in
      RyuData\\sdcard\\MikuMikuSwitchPlugin\\config.toml [multiplayer], or
    - server_mode = "cloud" for the public server.
* In the song-select screen press ZR once to open the multiplayer panel.
  ZR x3 cycles AutoPlay, ZR x5 toggles it off.


--------------------------------------------------------------
INSTALLING MOD UPDATES LATER
--------------------------------------------------------------
A new version of the mod is only ever TWO files:
    subsdk9   and   main.npdm
1. Close the game.
2. Copy both files into:
     RyuData\\mods\\contents\\{TID}\\mods\\exefs\\
   replacing the old ones. That folder must contain ONLY these two
   files - delete anything else in it (an old "subsdk8" will crash
   the game at boot).
3. Start the game again with "Play Mega Mix.bat".
Nothing else ever needs updating for a mod release.


--------------------------------------------------------------
TROUBLESHOOTING
--------------------------------------------------------------
* Game window opens off-screen or graphics look wrong:
    close the game, delete RyuData\\Config.json, start again
    (a fresh one is created - saves and mods are NOT affected).
* "missing keys" / decryption errors:
    the pack was not fully extracted - RyuData\\system\\prod.keys
    and title.keys must exist.
* The game file does not appear in Ryujinx's game list:
    always start with "Play Mega Mix.bat" - it launches the game
    directly. (Or add the Games folder under Options -> Settings
    -> General -> Game Directories.)
* Multiplayer never connects:
    look at RyuData\\sdcard\\hook_log.txt and the Ryujinx window
    title/log; check the server is running and reachable on the
    same Wi-Fi.
* Game freezes during the intro movie:
    known emulator issue - close and relaunch. If it keeps
    happening, send RyuData\\sdcard\\hook_log.txt and the newest
    file in Ryujinx\\Logs\\ to the mod author.
"""


# ------------------------------------------------------------------ main
def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", default=os.path.join(REPO, "pack_out"),
                   help="output dir (default pack_out/)")
    p.add_argument("--part-mb", type=int, default=4000,
                   help="max size per game-part zip in MB (default 4000)")
    p.add_argument("--name", default=None,
                   help="player name for this pack (default Emu-XXXX)")
    p.add_argument("--client-id", default=None,
                   help="16-hex client id (default: random)")
    p.add_argument("--nx-ip", default=DEFAULT_NX_IP,
                   help=f"Switch IP for FTP pull (env NX_IP, def {DEFAULT_NX_IP})")
    p.add_argument("--skip-build", action="store_true",
                   help="don't rebuild; use deploy/ or the installed exefs")
    p.add_argument("--skip-switch", action="store_true",
                   help="don't pull mods/config from the real Switch")
    p.add_argument("--skip-zip", action="store_true",
                   help="stage the tree but don't create zips")
    p.add_argument("--no-cache", action="store_true",
                   help="exclude the ~680 MB PTC/shader cache (slower first boot)")
    p.add_argument("--keep-stage", action="store_true",
                   help="keep the staged MegaMixPack/ tree after zipping")
    a = p.parse_args()

    for path, what in ((RYU_BIN, "Ryujinx binaries"), (NSP, "game NSP"),
                       (RYU_DATA, "Ryujinx data root")):
        if not os.path.exists(path):
            die(f"{what} not found at {path}")

    cid = (a.client_id or secrets.token_hex(8)).lower()
    if not re.fullmatch(r"[0-9a-f]{16}", cid):
        die("--client-id must be exactly 16 lowercase hex chars")
    name = a.name or f"Emu-{cid[-4:]}"

    stage_root = os.path.join(a.out, "stage")
    if os.path.exists(stage_root):
        shutil.rmtree(stage_root)
    os.makedirs(stage_root, exist_ok=True)

    if not a.skip_build:
        build_mod()

    pulled = None if a.skip_switch else \
        pull_switch_content(a.nx_ip, os.path.join(a.out, "tmp_switch"))

    root = stage(stage_root, a, cid, name, pulled)

    if a.skip_zip:
        log(f"--skip-zip: staged tree at {root}")
        if not a.keep_stage:
            log("(staging kept anyway when --skip-zip)")
        return

    log("creating zips ...")
    # pulled Switch trees get their own zip part(s), not the setup zip
    switch_rels = [rel for key, rel in SWITCH_STAGED
                   if os.path.isdir(os.path.join(root, rel))]
    z1 = os.path.join(a.out, "MegaMix_Pack_Setup.zip")
    zip_dir(root, z1,
            exclude=[f"{PACK_ROOT_NAME}/{rel}" for rel in switch_rels])
    with zipfile.ZipFile(z1, "a", allowZip64=True) as z:
        z.writestr("READ ME FIRST.txt",
                   "Extract EVERY MegaMix_Pack_*.zip into ONE folder, then\r\n"
                   "open  README - START HERE.txt  inside it.\r\n")
    zips = [z1]
    zips += zip_switch_mods(stage_root, switch_rels, a.out,
                          int(a.part_mb * 1_000_000))
    zips += zip_game_parts(NSP, a.out, a.part_mb)

    total = sum(os.path.getsize(z) for z in zips)
    log("=" * 60)
    log(f"DONE - {len(zips)} zip(s), {total/1e9:.2f} GB total, in {a.out}")
    for z in zips:
        log(f"  {os.path.basename(z)}")
    log(f"identity: name={name} client_id={cid}")
    if not pulled:
        warn("no Switch content included (console unreachable or --skip-switch)")
    if not a.keep_stage:
        shutil.rmtree(stage_root, ignore_errors=True)
        shutil.rmtree(os.path.join(a.out, "tmp_switch"), ignore_errors=True)
        log("staging removed (--keep-stage to retain)")


if __name__ == "__main__":
    main()
