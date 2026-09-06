# Release Notes — v0.1.0 (DRAFT, unreleased)

First production release of SpectralVisualizationGenerator (SpectraScope).

## What's included

- `spectragen` CLI: spectrogram/spectrum PNG, MP4/WebM video, batch mode
  (`batch <input> <output>`, recursive, bounded parallel jobs, retries,
  per-file results, Ctrl-C), `--gpu` rendering, `--multiband` comparison.
- `spectra_gui` Qt6 desktop app: file select + drag-drop, 3 analysis presets,
  PNG preview, progress, open-output-location, GPU toggle.
- Shared `run_job` pipeline: CLI and GUI produce equivalent results by
  construction (byte-identical outputs pinned by test).

## Verification (measured, Release build, Ryzen 5 7520U / Win11)

- Clean rebuild, zero errors; `spectragen --help` / `--version` (0.1.0) work.
- 21/21 ctest suites pass (unit, integration, accuracy, regression,
  MSVC ASan smoke, stress).
- GPU-vs-CPU rendering: PSNR 61–66 dB, SSIM 1.0; GPU wins at 1024×512+.
- Portable tree smoke-tested with stripped PATH (GUI starts, CLI renders
  with FFmpeg present).

## Requirements

- Windows 10/11 64-bit; user-supplied FFmpeg on PATH; VC++ Redistributable
  if DLL errors appear.

## Known limitations

See `docs/release/limitations.md`. Headliners: no clean-machine install test
yet, no installer (portable ZIP only), GPU FFT stub, Qt/FFmpeg distribution
rules pending legal review (`docs/licensing.md`).

## Distribution

Portable: `dist/SpectraScope-portable-win64.zip`. No GitHub release created.
