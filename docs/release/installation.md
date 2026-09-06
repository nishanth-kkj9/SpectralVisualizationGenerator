# Installation — v0.1.0 (Windows 64-bit)

## Requirements

- Windows 10/11 64-bit
- [FFmpeg](https://ffmpeg.org/) on PATH (`ffmpeg` + `ffprobe`) — **required**,
  user-supplied, never bundled (GPL build notes in `docs/licensing.md`)
- Visual C++ Redistributable (only if `MSVCP140.dll` errors appear)

## Portable (recommended)

1. Unzip `SpectraScope-portable-win64.zip` anywhere.
2. Run `spectra_gui.exe` (desktop app) or `spectragen.exe` (CLI).
3. No admin rights needed. No registry changes.

Verify: `spectragen --version` prints `0.1.0`; the GUI opens and stays open.

## From source

1. Install [vcpkg](https://vcpkg.io/) and Qt 6 via the project manifest
   (`vcpkg.json` — `qtbase` with `gui`, `widgets`).
2. `cmake -S . -B build "-DQt6_DIR=<repo>/vcpkg_installed/x64-windows/share/Qt6"`
3. `cmake --build build --config Release`
4. Binaries land in `build/Release/`.

## Clean-environment note

The portable tree was smoke-tested with a stripped `PATH`
(`C:\Windows\System32;C:\Windows` only): the GUI starts and the CLI renders
once FFmpeg is on PATH. It was **not** verified on a separate clean Windows
installation — treat first install on a new machine as a pilot run.
