# Launch MM+ via Steam and place it on DISPLAY2 as a true-16:9 1920x1080
# borderless window at (1920,60) — NOT maximized, so the 1920x1080
# swapchain maps 1:1 (a 1920x1200 window stretches the HUD vertically and
# leaves the taskbar visible on top of the frame).
#
# Never use `cmd /c start steam://...`: when the URL-handler dispatch
# fails, `start` spams "pick an app" dialogs.
#
# Usage: python tools/launch_game.py [--monitor-x 1920] [--monitor-y 60]
#                                    [--width 1920] [--height 1080]
#                                    [--no-launch]
import ctypes, subprocess, sys, time
from ctypes import wintypes as W

STEAM = r"C:\Program Files (x86)\Steam\steam.exe"
APPID = "1761390"

MX, MY, MW, MH = 1920, 60, 1920, 1080
for i, a in enumerate(sys.argv):
    if a == "--monitor-x" and i + 1 < len(sys.argv): MX = int(sys.argv[i + 1])
    if a == "--monitor-y" and i + 1 < len(sys.argv): MY = int(sys.argv[i + 1])
    if a == "--width"     and i + 1 < len(sys.argv): MW = int(sys.argv[i + 1])
    if a == "--height"    and i + 1 < len(sys.argv): MH = int(sys.argv[i + 1])

u = ctypes.windll.user32
CB = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
SW_RESTORE = 9
SWP_FRAMECHANGED = 0x20
HWND_TOP = 0


def game_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DivaMegaMix.exe",
                          "/FO", "CSV", "/NH"], capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        if "DivaMegaMix" in line:
            return int(line.split(",")[1].strip('"'))
    return None


def game_hwnds(pid):
    out = []
    def cb(h, l):
        p = W.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            out.append(h)
        return True
    u.EnumWindows(CB(cb), 0)
    return out


def place(h):
    """Move (do NOT restyle) the game's own framed window so its CLIENT
    area is MWxMH at (MX,MY) on DISPLAY2. The game is windowed per its
    own settings; we only reposition it. No maximize, no WS_POPUP."""
    if u.IsIconic(h):
        u.ShowWindow(h, SW_RESTORE)
    st = u.GetWindowLongW(h, GWL_STYLE)
    ex = u.GetWindowLongW(h, GWL_EXSTYLE)
    rc = W.RECT(0, 0, MW, MH)
    u.AdjustWindowRectEx(ctypes.byref(rc), st, False, ex)
    u.SetWindowPos(h, HWND_TOP, MX + rc.left, MY + rc.top,
                   rc.right - rc.left, rc.bottom - rc.top, SWP_FRAMECHANGED | 0x10)  # + SWP_NOACTIVATE


def move_console_to_display2():
    """The DML console window (title contains 'DivaMegaMix.exe') sits on
    DISPLAY1; park it on DISPLAY2 as well."""
    def cb(h, l):
        if not u.IsWindowVisible(h):
            return True
        t = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, t, 256)
        if "DivaMegaMix.exe" in t.value and not u.IsIconic(h):
            # keep it out of the way: minimized, never activated. A click
            # on a console in QuickEdit mode pauses its writers.
            u.ShowWindow(h, 7)   # SW_SHOWMINNOACTIVE
        return True
    u.EnumWindows(CB(cb), 0)


if "--no-launch" not in sys.argv:
    subprocess.Popen([STEAM, "-applaunch", APPID])

# move the real game window (titled "...Project DIVA Mega Mix+") and the
# DML console to DISPLAY2.
# The game applies its own saved window size after creating the window, so
# keep re-placing until the client rect has been right for 5 s (or 90 s).
deadline = time.time() + 90
good_since = None
while time.time() < deadline:
    pid = game_pid()
    if pid:
        for h in game_hwnds(pid):
            t = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(h, t, 256)
            if not t.value.startswith("Hatsune Miku:"):
                continue
            rc = W.RECT(); u.GetClientRect(h, ctypes.byref(rc))
            pt = W.POINT(0, 0); u.ClientToScreen(h, ctypes.byref(pt))
            if (rc.right, rc.bottom, pt.x, pt.y) == (MW, MH, MX, MY):
                good_since = good_since or time.time()
            else:
                good_since = None
                place(h)
                print(f"placed hwnd {h} -> client ({MX},{MY}) {MW}x{MH}", flush=True)
    move_console_to_display2()
    if good_since and time.time() - good_since > 5:
        break
    time.sleep(0.25)
print("done")
