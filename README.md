# Spectral Visualization Generator (SpectraScope)

Offline file-to-file spectral rendering: audio/video in, spectrogram/spectrum
images and videos out. Not a music player, DAW, real-time visualizer, or AI app.

## Binaries

- `spectragen` — CLI: single jobs + `batch` mode (folders, parallel jobs,
  retries, per-file results). See `docs/release/cli-usage.md`.
- `spectra_gui` — Qt6 desktop thin client over the same pipeline.
  See `docs/release/gui-usage.md`.

## Workflow

Audio/Video file → Media decoding → Audio processing → Spectral analysis →
Spectral data → Spectral visualization → Image/video output

## Build (Windows 64-bit)

Requires FFmpeg on PATH (`ffmpeg` + `ffprobe`, user-supplied) and Qt 6 via
the vcpkg manifest (`vcpkg.json`):

```powershell
cmake --preset windows-release
cmake --build build --config Release
ctest --test-dir build -C Release
```

Binaries land in `build/Release/`. Portable packaging:
`dist/package_portable.ps1`. Details in `docs/release/installation.md`.

## Docs

- User: `docs/release/` (installation, CLI/GUI usage, formats, methods,
  reproducibility, accuracy + benchmark methodology, limitations)
- Licensing: `docs/licensing.md`, `THIRD-PARTY-NOTICES.md`
- Architecture: `docs/architecture/ARCHITECTURE.md`
