# Spectral Visualization Generator (SpectraScope)

Offline file-to-file spectral rendering: audio/video in, spectrogram/spectrum
images and videos out. Not a music player, DAW, real-time visualizer, or AI app.

## Binaries

| Binary | What it is | Doc |
|---|---|---|
| `spectragen` | CLI: single jobs + `batch` mode (folders, parallel jobs, retries, per-file results) | `docs/cli-usage.md` |
| `spectra_gui` | Qt6 desktop thin client over the same pipeline | `docs/gui-usage.md` |
| `media_probe` | FFmpeg decode probe utility | `docs/analysis-methods.md` |
| `spectral_benchmarks` | Benchmark runner (JSON output) | `docs/benchmark-methodology.md` |

## Workflow

Audio/Video file → Media decoding → Audio processing → Spectral analysis →
Spectral data → Spectral visualization → Image/video output

## Layout

```text
apps/          entry points (spectragen CLI, spectrascope GUI, media-probe)
src/core/      pipeline, DSP, spectral, rendering, media, encoding, batch, project
src/benchmarks benchmark sources
tests/         feature suites (dsp, rendering, spectral, video, cli, accuracy) + golden/
tools/packaging portable packaging scripts
docs/          user docs + architecture/ + archive/ (phase history)
benchmarks/    benchmark run outputs (git-ignored)
dist/          portable packaging output (git-ignored)
```

## Prerequisites

- Windows 10/11 64-bit, Visual Studio 2022 17.8+ (or 18 2026), CMake 3.20+
- [FFmpeg](https://ffmpeg.org/) on PATH (`ffmpeg` + `ffprobe`) — **required**,
  user-supplied, never bundled (see `docs/licensing.md`)
- Qt 6 comes via the vcpkg manifest (`vcpkg.json` — `qtbase` with `gui`, `widgets`)

## Build

The checked-in presets resolve the vcpkg toolchain from `VCPKG_ROOT`, so
point it at your vcpkg checkout first (the `Visual Studio 18 2026` preset
also needs CMake 4.0+; older tooling can configure manually as CI does):

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset windows-release
cmake --build build --config Release
ctest --test-dir build -C Release
```

The full registered suite must pass (`ctest -N` lists the tests your build
registered — optional targets such as the Qt GUI lifecycle and sanitizer
tests depend on the environment). Binaries land in `build/Release/`.
Portable packaging: `tools/packaging/package_portable.ps1`.
Details in `docs/installation.md`.

## Docs

- User: `docs/` (installation, CLI/GUI usage, formats, methods,
  reproducibility, accuracy + benchmark methodology, limitations)
- Licensing: `docs/licensing.md`, `THIRD-PARTY-NOTICES.md`
- Architecture: `docs/architecture/SpectraScope_Architecture_2026-11-26.md`,
  `docs/architecture/spectral-representations.md`
- History: `docs/archive/` (phase plans, specs, notes)

## CI

`.github/workflows/ci.yml` builds Release and runs the full suite on every
push to `main` and every pull request.

## License

MIT — see `LICENSE`. Third-party notices in `THIRD-PARTY-NOTICES.md`.
