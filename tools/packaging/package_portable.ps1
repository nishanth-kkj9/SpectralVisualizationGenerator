# Phase 22 — Assemble portable dist/ from the Release build + vcpkg Qt.
# Usage: powershell -ExecutionPolicy Bypass -File tools/packaging/package_portable.ps1
# Output: dist/SpectraScope-portable/ (+ .zip). No installer tooling required.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root "dist/SpectraScope-portable"
$qtBin = Join-Path $root "vcpkg_installed/x64-windows/bin"
$qtPlugins = Join-Path $root "vcpkg_installed/x64-windows/Qt6/plugins"

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
New-Item -ItemType Directory (Join-Path $out "plugins/platforms") | Out-Null

Copy-Item (Join-Path $root "build/Release/spectra_gui.exe") $out
Copy-Item (Join-Path $root "build/Release/spectragen.exe") $out
Copy-Item (Join-Path $qtPlugins "platforms/qwindows.dll") (Join-Path $out "plugins/platforms")

# Transitive DLL closure over vcpkg bin (system DLLs resolve from WinSxS).
$queue = @("spectra_gui.exe", "spectragen.exe")
$seen = @{}
$vcpkgDlls = @{}
Get-ChildItem $qtBin -Filter "*.dll" | ForEach-Object { $vcpkgDlls[$_.Name.ToLower()] = $_.FullName }
while ($queue.Count -gt 0) {
    $item = $queue[0]
    if ($queue.Count -eq 1) { $queue = @() } else { $queue = $queue[1..($queue.Count - 1)] }
    if ($seen.ContainsKey($item.ToLower())) { continue }
    $seen[$item.ToLower()] = $true
    $path = if ([IO.Path]::IsPathRooted($item)) { $item } else { Join-Path $out $item }
    if (-not (Test-Path $path)) { $path = $vcpkgDlls[$item.ToLower()] }
    if (-not $path -or -not (Test-Path $path)) { continue }
    dumpbin /dependents "$path" 2>$null | ForEach-Object {
        if ($_ -match "^\s+([A-Za-z0-9_+-]+\.dll)\s*$") {
            $dll = $Matches[1]
            $key = $dll.ToLower()
            if (-not $seen.ContainsKey($key) -and $vcpkgDlls.ContainsKey($key)) {
                Copy-Item $vcpkgDlls[$key] $out -Force
                $queue += $dll
            }
        }
    }
}

# qt.conf pins plugin lookup to the portable tree.
"[Paths]`nPrefix = .`nPlugins = plugins`n" | Out-File (Join-Path $out "qt.conf") -Encoding ascii

Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.md") (Join-Path $out "THIRD-PARTY-NOTICES.txt")
if (Test-Path (Join-Path $root "LICENSE")) {
    Copy-Item (Join-Path $root "LICENSE") (Join-Path $out "LICENSE.txt")
}

$zip = Join-Path $root "dist/SpectraScope-portable-win64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $out -DestinationPath $zip
Write-Output "dist ready: $out"
Write-Output "zip ready: $zip"
Write-Output ("files: " + (Get-ChildItem $out | Measure-Object).Count)
