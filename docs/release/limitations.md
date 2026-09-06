# Limitations — v0.1.0

- **FFmpeg required**: no bundled decoder; without `ffmpeg`/`ffprobe` on PATH
  every job fails (clearly, exit 3).
- **Windows only**: D3D11 GPU path and subprocess handling target Windows 10/11.
- **No clean-machine install verification**: portable tree smoke-tested with a
  stripped PATH only; first install on a fresh machine is a pilot run.
- **No installer**: portable ZIP + script only; no Inno/NSIS installer yet.
- **GPU FFT absent**: `gpu_backend` is a stub; GPU covers spectrogram rendering only.
- **32-bit float DSP** throughout; very long files need proportional RAM
  (full decode buffered; OOM fails as analysis error, not streaming).
- **Legal**: Qt license selection and FFmpeg bundling rules unresolved —
  see `docs/licensing.md`. Do not redistribute Qt DLLs or GPL FFmpeg builds
  without completing that review.
- GUI has no cancel button for in-flight jobs (CLI supports Ctrl-C).
