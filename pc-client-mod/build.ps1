# MegaMixMP build: vswhere -> vcvars64 -> Windows CMake (Ninja) -> dist/
param([switch]$Dev)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { $vswhere = "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe" }
if (-not (Test-Path $vswhere)) { throw "vswhere not found" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "no Visual Studio with C++ tools found" }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Windows CMake only (the msys/devkitPro cmake on PATH is a different dialect)
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { throw "Windows cmake not found at $cmake" }

$ninja = "$env:APPDATA\Python\Python313\Scripts\ninja.exe"
if (-not (Test-Path $ninja)) { $ninja = 'ninja' }

$devArg = if ($Dev) { ' -DMP_DEV_TOOLS=ON' } else { '' }
& $env:ComSpec /c "`"$vcvars`" && `"$cmake`" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release$devArg -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build build"
if ($LASTEXITCODE -ne 0) { Pop-Location; exit $LASTEXITCODE }

New-Item -ItemType Directory -Force 'dist\MegaMixMP' | Out-Null
Copy-Item -Force 'build\MegaMixMP.dll' 'dist\MegaMixMP\MegaMixMP.dll'
Write-Host "OK -> dist\MegaMixMP\MegaMixMP.dll"
Pop-Location
