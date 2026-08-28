# Build a self-contained, copyable folder: the game, its assets and every runtime DLL that
# is not part of Windows. Copy the whole folder to the other PC -- both machines then have
# byte-identical assets, which is what the LAN JOIN handshake's data hash checks.
#
#   powershell -ExecutionPolicy Bypass -File port/tools/make_dist.ps1
#   ... -Out D:\TJ-LAN         where to build it (default: dist\TomJerryWOW-LAN)
#   ... -Width 1280 -Height 720 -Mode 0     the shipped tomjerry.ini display defaults
#   ... -NoBuild               skip the compile, just re-stage
param(
  [string]$Out = "",
  [int]$Width = 1280,
  [int]$Height = 720,
  [int]$Mode = 0,             # 0 windowed, 1 borderless, 2 exclusive fullscreen
  [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$root = "D:\Projects\Tom and Jerry in War of the Whiskers (U)"
$bin  = "$root\port\build\bin\Release"
if ($Out -eq "") { $Out = "$root\dist\TomJerryWOW-LAN" }

if (-not $NoBuild) {
  Write-Host "== building =="
  Get-Process tj_loader -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 300
  & cmd.exe /c "$root\port\tools\build_hybrid.cmd" | Select-String -Pattern "error|\.dll|\.exe" |
    ForEach-Object { $_.Line }
  if (-not (Test-Path "$bin\tj_hybrid.dll")) { throw "build produced no tj_hybrid.dll" }
}

Write-Host "== staging into $Out =="
# KEEP THE PLAYER'S SAVE. Re-staging used to delete the whole folder, which took _SAVES with
# it -- so every rebuild wiped the save game in an install someone was actually playing. The
# save is moved aside, the folder is rebuilt clean as before, and the save is put back.
$saveKeep = $null
if (Test-Path "$Out\_SAVES") {
  $saveKeep = Join-Path ([System.IO.Path]::GetTempPath()) ("tj_saves_" + [System.Guid]::NewGuid().ToString("N"))
  Move-Item "$Out\_SAVES" $saveKeep
  $n = (Get-ChildItem $saveKeep -Recurse -File | Measure-Object).Count
  Write-Host "   keeping the existing save ($n file(s))"
}
if (Test-Path $Out) { Remove-Item $Out -Recurse -Force }
New-Item -ItemType Directory -Path $Out -Force | Out-Null
if ($saveKeep) { Move-Item $saveKeep "$Out\_SAVES" }

# 1. the port itself
Copy-Item "$bin\tj_loader.exe","$bin\tj_hybrid.dll" $Out
# 1b. the Arabic pack, when this machine has built one (tools/arabic_font.py --pack).
#     Optional by design: with no pack the game simply stays English.
$arabic = Join-Path $PSScriptRoot "..\build-arabic\arabic_font.bin"
if (Test-Path $arabic) {
  Copy-Item $arabic $Out
  Write-Host "   + arabic_font.bin ($([math]::Round((Get-Item $arabic).Length/1KB)) KB)"
} else {
  Write-Host "   (no Arabic pack -- the dist folder will be English-only)"
}

# 2. runtime DLLs Windows does NOT ship. The UCRT (api-ms-win-crt-*), d3d11, dxgi, ws2_32,
#    xinput1_4 and xaudio2_9 are all inbox on Windows 10/11; the VC++ runtime is not, and
#    app-local copies are the supported way to avoid making the other PC install a redist.
$bt = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$crt = Get-ChildItem "$bt\VC\Redist\MSVC\*\x86\Microsoft.VC*.CRT" -Directory |
       Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "no x86 VC++ redist found under $bt" }
foreach ($d in @("msvcp140.dll","msvcp140_1.dll","msvcp140_2.dll","msvcp140_atomic_wait.dll",
                 "vcruntime140.dll","vcruntime140_threads.dll","concrt140.dll")) {
  if (Test-Path "$($crt.FullName)\$d") { Copy-Item "$($crt.FullName)\$d" $Out }
}
# d3dcompiler_47 IS inbox on Win10+, but shipping the 32-bit copy costs 4 MB and removes the
# one plausible "works here, not there" failure.
if (Test-Path "C:\Windows\SysWOW64\d3dcompiler_47.dll") {
  Copy-Item "C:\Windows\SysWOW64\d3dcompiler_47.dll" $Out
}

# 3. the game: default.xbe + the three asset trees. These, and ONLY these, are what the LAN
#    data hash covers, so both PCs must have them identical -- copying this folder is what
#    guarantees that.
Copy-Item "$root\extracted\default.xbe" $Out
foreach ($sub in @("GFX","AUDMUSIC","AUDSoundFX")) {
  Copy-Item "$root\extracted\$sub" $Out -Recurse
}
New-Item -ItemType Directory -Path "$Out\_SAVES" -Force | Out-Null   # the game writes here

# 3b. MEAT RUSH needs the same turkey leg in every arena, and `WPH2` is a per-arena NAME SLOT
#     (a bone in Hell, a rapier in Cabin, absent in seven arenas), so KITCHEN's mesh is copied
#     into each arena's object file under the name MEAT. Idempotent; run on the staged copy so
#     the repo's extracted/ tree and the shipped one always agree. Changing assets changes the
#     LAN data hash -- both PCs must be installed from the SAME build.
& python "$root\port\tools\inject_meat.py" $Out
if ($LASTEXITCODE -ne 0) { throw "inject_meat.py failed on the staged assets" }

# 4. settings. No [Player] Name is shipped: each PC picks up its own computer name the first
#    time, so the two lobby rows are not both called the same thing.
@"
[Display]
Width=$Width
Height=$Height
Mode=$Mode
"@ | Set-Content "$Out\tomjerry.ini" -Encoding ascii

# 5. launcher + instructions
@"
@echo off
cd /d "%~dp0"
start "" tj_loader.exe m4
"@ | Set-Content "$Out\PLAY.cmd" -Encoding ascii

Copy-Item "$root\port\tools\dist_README.txt" "$Out\README.txt"

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object -Property Length -Sum).Sum
$count = (Get-ChildItem $Out -Recurse -File).Count
Write-Host ""
Write-Host ("== done: {0} files, {1:N0} MB ==" -f $count, ($size / 1MB))
Write-Host $Out
