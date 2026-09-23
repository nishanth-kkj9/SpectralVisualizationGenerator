# Limitations — v0.1.0

- **FFmpeg required**: no bundled decoder; without `ffmpeg`/`ffprobe` on PATH
  every job fails (clearly, exit 3).
- **Windows only**: D3D11 GPU path and subprocess handling target Windows 10/11.
- **No clean-machine install verification**: portable tree smoke-tested with a
  stripped PATH only; first install on a fresh machine is a pilot run.
- **No installer**: portable ZIP + script only; no Inno/NSIS installer yet.
- **GPU FFT absent**: `gpu_backend` is a stub; GPU covers spectrogram rendering only.
- **32-bit float DSP** throughout; raw input audio is streamed with bounded
  memory (~2·fft + one decoder chunk live), but the `SpectralDataset`
  itself still grows with the number of spectral frames, so very long
  files need proportional RAM for the dataset — see
  `docs/media-ingestion.md` (OOM fails as analysis error).
- **Legal**: Qt license selection and FFmpeg bundling rules unresolved —
  see `docs/licensing.md`. Do not redistribute Qt DLLs or GPL FFmpeg builds
  without completing that review.
- **Only one filterbank representation**: Mel (triangular filterbank over
  STFT power) is implemented; Bark, ERB and CQT exist as display scales only.
  CQT is not a representation at all yet.
- **Non-STFT datasets are JSON-only**: the dataset binary format (v3) is
  STFT-only by contract and fails closed for Mel data (a v4 binary is a
  deliberate future decision), so Mel datasets persist and reload through
  JSON.
- **GPU is STFT-rendering only**: no GPU Mel computation or rendering, and
  none is claimed; `--gpu` has no effect on a Mel run (CPU Mel renderer).
- **Mel bands are coarser than FFT bins by design**: Mel centroid/bandwidth
  and pixel rows resolve only to the Mel center grid, so Mel images are
  intentionally lower-resolution in frequency than an STFT image of the same
  FFT size.
- GUI has no cancel button for in-flight jobs (CLI supports Ctrl-C).
- **Output replacement is crash-safe, not power-safe**: new bytes go to a
  uniquely named `<stem>.<pid>.<ctr>.part<ext>` temp file beside the
  destination and the OS swaps it over the old file (never delete-then-
  rename), so a failed job keeps the previous valid output. A hard crash
  or power loss can still leave a stale `.part.*` temp behind; such files
  are never treated as final outputs and never picked up by batch scans,
  but no background scavenger removes them — delete them manually.
- **Replacement needs a cooperating filesystem**: if the destination is
  open without sharing (e.g. in another program), replacement fails with
  a sharing-violation error and the old file is kept.
- **Single-job runs do not create missing output parents** (batch mode
  does); a missing parent fails at render/encode time with a typed error.
- **Cancellation latency is bounded, not zero**: decode observes a request
  within one pipe poll (~5ms); analysis/rendering/video stop at frame/row
  boundaries; ffprobe and an in-flight GPU dispatch run to completion
  first (both are short). No GUI test harness exists, so Cancel-button
  behavior is verified by build plus code review, not by an automated test.
