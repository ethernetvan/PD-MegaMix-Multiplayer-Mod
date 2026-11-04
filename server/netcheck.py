"""Startup reachability hints: the PC's LAN addresses and, on Windows, a
best-effort check that Windows Firewall lets consoles reach this Python
(an inbound Block rule, or no Allow rule for the active network profile,
silently breaks discovery and connects)."""

import ipaddress
import json
import os
import socket
import subprocess
import sys


def lan_ipv4():
    addrs = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None,
                                       socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    try:  # address of the default route (no packet is sent)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            addrs.add(s.getsockname()[0])
    except OSError:
        pass
    return sorted((a for a in addrs
                   if not a.startswith(("127.", "169.254.", "0."))),
                  key=ipaddress.IPv4Address)


def _as_list(v):
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def firewall_problems(profiles, rules):
    """profiles: active NetworkCategory names (Public/Private/
    DomainAuthenticated); rules: enabled inbound rules for our exe as
    [{"action": "Allow"|"Block", "profile": "Any"|"Public, Private"...}].
    Returns one message per uncovered profile (Block beats Allow)."""
    out = []
    for cat in dict.fromkeys(_as_list(profiles)):
        prof = "Domain" if cat == "DomainAuthenticated" else cat

        def covers(r):
            p = str(r.get("profile", ""))
            return p == "Any" or prof in [x.strip() for x in p.split(",")]

        if any(r.get("action") == "Block" and covers(r) for r in rules):
            out.append(f"an inbound Block rule for this Python covers the "
                       f"{prof} network profile")
        elif not any(r.get("action") == "Allow" and covers(r)
                     for r in rules):
            out.append(f"no inbound Allow rule for this Python on the "
                       f"{prof} network profile")
    return out


_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
$prof = @(Get-NetConnectionProfile | ForEach-Object { [string]$_.NetworkCategory })
$rules = @(Get-NetFirewallApplicationFilter | Where-Object { $_.Program -eq $env:MP_EXE } |
  Get-NetFirewallRule | Where-Object { [string]$_.Enabled -eq 'True' -and [string]$_.Direction -eq 'Inbound' } |
  ForEach-Object { @{ action = [string]$_.Action; profile = [string]$_.Profile } })
ConvertTo-Json -Compress -Depth 4 @{ profiles = $prof; rules = $rules }
"""


def parse_ps(text):
    d = json.loads(text)
    return _as_list(d.get("profiles")), [r for r in _as_list(d.get("rules"))
                                         if isinstance(r, dict)]


def check_firewall(log):
    if sys.platform != "win32":
        return
    exe = os.path.realpath(sys.executable)
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", _PS],
            capture_output=True, text=True, timeout=20,
            env={**os.environ, "MP_EXE": exe},
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        profiles, rules = parse_ps(r.stdout.strip())
    except Exception as e:
        log(f"firewall check skipped ({type(e).__name__})")
        return
    probs = firewall_problems(profiles, rules)
    if not probs:
        log(f"firewall: OK for {', '.join(profiles) or 'no active network'}")
        return
    for p in probs:
        log(f"WARNING firewall: {p} - consoles may not find or reach "
            f"this server")
    log("  fix (elevated PowerShell): New-NetFirewallRule -DisplayName "
        f"\"MegaMix MP server\" -Direction Inbound -Program \"{exe}\" "
        "-Action Allow -Profile Any")
    if any("Block" in p for p in probs):
        log("  and remove the Block rule: Get-NetFirewallApplicationFilter | "
            f"? Program -eq \"{exe}\" | Get-NetFirewallRule | "
            "? Action -eq Block | Disable-NetFirewallRule")
