# Phase 22 — Portable install helper (no admin, no registry).
# Usage: powershell -ExecutionPolicy Bypass -File dist/install.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "dist/SpectraScope-portable"
if (-not (Test-Path $src)) { throw "Run dist/package_portable.ps1 first." }
$dst = Join-Path $env:LocalAppData "SpectraScope"
Remove-Item $dst -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $src $dst -Recurse
Write-Output "Installed to $dst"
Write-Output "Requires ffmpeg/ffprobe on PATH (user-supplied, see docs/release/installation.md)."
