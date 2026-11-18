# Deploy: junction "<game>\mods\MegaMixMP" -> pc-client-mod\dist\MegaMixMP
# Idempotent; refuses if a real (non-junction) directory already exists there.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$game  = 'C:\Program Files (x86)\Steam\steamapps\common\Hatsune Miku Project DIVA Mega Mix Plus'
$mods  = Join-Path $game 'mods'
$link  = Join-Path $mods 'MegaMixMP'
$dst   = Join-Path $root 'dist\MegaMixMP'

if (-not (Test-Path $dst)) { throw "dist\MegaMixMP not built yet - run build.ps1 first" }
if (-not (Test-Path (Join-Path $dst 'MegaMixMP.dll'))) { throw "MegaMixMP.dll missing in dist - run build.ps1" }
New-Item -ItemType Directory -Force $mods | Out-Null

if (Test-Path $link) {
    $item = Get-Item $link -Force
    if ($item.LinkType -eq 'Junction') {
        if ($item.Target -eq $dst) { Write-Host "junction already ok: $link -> $dst"; exit 0 }
        Remove-Item $link -Force -Recurse   # re-point an old junction
    } else {
        throw "$link is a real directory - refusing to overwrite"
    }
}
New-Item -ItemType Junction -Path $link -Target $dst | Out-Null
Write-Host "junction: $link -> $dst"
