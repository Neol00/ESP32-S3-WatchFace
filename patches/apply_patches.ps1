<#
.SYNOPSIS
  Apply the ESP32-S3-WatchFace library patches (LVGL, Arduino_GFX, ESP32 core BLE).

.DESCRIPTION
  Applies the 6 unified-diff patches in this folder to your local libraries. Dry-runs
  every patch first and aborts if ANY would not apply cleanly, so it never leaves your
  tree half-patched. Skips patches that are already applied.

  Requires `git` (used as a portable patch tool via `git apply`; present with Arduino's
  toolchain and on most dev machines).

.PARAMETER LibrariesDir
  Your Arduino libraries folder (contains `lvgl`, `lv_conf.h`, `GFX_Library_for_Arduino`).
  Default: $HOME\Documents\Arduino\libraries

.PARAMETER Esp32CoreDir
  The installed ESP32 core root (contains `libraries\BLE`).
  Default: $env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\3.3.8

.EXAMPLE
  ./apply_patches.ps1
  ./apply_patches.ps1 -LibrariesDir "D:\Arduino\libraries" -Esp32CoreDir "D:\esp32\3.3.8"
#>
[CmdletBinding()]
param(
  [string]$LibrariesDir = (Join-Path $HOME "Documents\Arduino\libraries"),
  [string]$Esp32CoreDir = (Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32\3.3.8")
)

$ErrorActionPreference = "Stop"
$patchDir = $PSScriptRoot

# Each entry: patch file, the directory to apply it from (-p1 root).
$gfxDir = Join-Path $LibrariesDir "GFX_Library_for_Arduino"
$lvglDir = Join-Path $LibrariesDir "lvgl"
$bleDir  = Join-Path $Esp32CoreDir "libraries\BLE"
$i2sDir  = Join-Path $Esp32CoreDir "libraries\ESP_I2S"

$jobs = @(
  @{ Patch = "01-lvgl-freertos-corepin.patch"; Root = $lvglDir;      Name = "LVGL render-thread core pin" },
  @{ Patch = "02-lv_conf-snapshot.patch";      Root = $LibrariesDir; Name = "lv_conf.h snapshot enable" },
  @{ Patch = "03-gfx-qspi-dma.patch";          Root = $gfxDir;       Name = "GFX QSPI async-DMA flush" },
  @{ Patch = "04-gfx-qspi-header.patch";       Root = $gfxDir;       Name = "GFX QSPI second transaction struct" },
  @{ Patch = "05-esp32-ble-gap.patch";         Root = $bleDir;       Name = "ESP32 core BLE GAP-listener unregister" },
  @{ Patch = "06-esp32-psram-size.patch";      Root = $Esp32CoreDir; Name = "ESP32 core getPsramSize physical chip size" },
  @{ Patch = "08-esp32-i2s-channel-leak.patch"; Root = $i2sDir;      Name = "ESP32 core ESP_I2S channel-leak fix" }
)

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  throw "git not found on PATH. Install git, or apply the patches manually (see README.md)."
}

# Validate target dirs exist.
foreach ($d in @($lvglDir, $LibrariesDir, $gfxDir, $bleDir, $i2sDir)) {
  if (-not (Test-Path $d)) { throw "Target directory not found: $d`nCheck -LibrariesDir / -Esp32CoreDir." }
}

function Test-PatchState {
  param($PatchPath, $Root)
  # Returns 'applies', 'applied', or 'fails'.
  # A failing `git apply --check` is EXPECTED here (that's how we detect state), but
  # under Windows PowerShell 5.1 with $ErrorActionPreference=Stop, ANY native stderr
  # becomes a terminating NativeCommandError regardless of redirection. Lower the
  # preference around the git calls so stderr is just discarded.
  $prevEAP = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & git -C $Root apply --check $PatchPath 2>$null
    if ($LASTEXITCODE -eq 0) { return "applies" }
    & git -C $Root apply --reverse --check $PatchPath 2>$null
    if ($LASTEXITCODE -eq 0) { return "applied" }
    return "fails"
  } finally {
    $ErrorActionPreference = $prevEAP
  }
}

Write-Host "ESP32-S3-WatchFace — applying library patches`n" -ForegroundColor Cyan
Write-Host "  libraries: $LibrariesDir"
Write-Host "  esp32 core: $Esp32CoreDir`n"

# --- Phase 1: dry-run everything, decide, abort on any hard failure ---
$plan = @()
$hardFail = $false
foreach ($j in $jobs) {
  $pp = Join-Path $patchDir $j.Patch
  if (-not (Test-Path $pp)) { throw "Missing patch file: $pp" }
  $state = Test-PatchState -PatchPath $pp -Root $j.Root
  $plan += [pscustomobject]@{ Job = $j; State = $state }
  switch ($state) {
    "applies" { Write-Host ("  [will apply] {0}" -f $j.Name) -ForegroundColor Green }
    "applied" { Write-Host ("  [already ok] {0}" -f $j.Name) -ForegroundColor Yellow }
    "fails"   { Write-Host ("  [CANNOT APPLY] {0}  ({1})" -f $j.Name, $j.Patch) -ForegroundColor Red; $hardFail = $true }
  }
}

if ($hardFail) {
  Write-Host "`nAborting: at least one patch does not apply cleanly. Nothing was changed." -ForegroundColor Red
  Write-Host "Most likely your library version differs. Required: LVGL 9.5.0, Arduino_GFX 1.6.5, ESP32 core 3.3.8." -ForegroundColor Red
  exit 1
}

# --- Phase 2: apply the ones that need applying ---
Write-Host ""
foreach ($p in $plan) {
  if ($p.State -ne "applies") { continue }
  $pp = Join-Path $patchDir $p.Job.Patch
  $prevEAP = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $gitOut = & git -C $p.Job.Root apply $pp 2>&1
  $ErrorActionPreference = $prevEAP
  if ($LASTEXITCODE -ne 0) { throw "Failed applying $($p.Job.Patch) despite passing dry-run: $gitOut" }
  Write-Host ("  applied: {0}" -f $p.Job.Name) -ForegroundColor Green
}

Write-Host "`nDone. Now clear the Arduino build cache and rebuild:" -ForegroundColor Cyan
Write-Host '  Remove-Item -Recurse -Force "$env:LOCALAPPDATA\arduino\sketches\*"'
