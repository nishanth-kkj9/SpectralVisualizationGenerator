# GUI Usage — SpectraScope

Thin client over the same pipeline as the CLI. No editor/player features.

1. **Select/drop** audio or video (Browse or drag onto the window).
2. **Visualization**: Spectrogram or Spectrum; output format Image (PNG) or
   Video (MP4). The format combo always decides: a typed output path whose
   extension contradicts it (e.g. Image + `.mp4`) is rejected before the
   job starts.
3. **Representation** selects WHAT is computed — unlike the frequency-scale
   combo, which only remaps the display. **STFT (FFT bins)** keeps the
   uniform FFT grid; **Mel filterbank** aggregates STFT power through a
   real triangular Mel filterbank into explicit bands. With Mel selected:
   - **Mel bands** (filterbank band count) and **Mel normalization**
     (none / slaney / area) become active;
   - **Spectrum** visualization is disabled (the spectrum plot is
     STFT-only) and **Reassignment** is disabled (Mel data is phaseless).
4. **Preset**: Voice (1024/hann), Music (2048/hann), Detail
   (4096/blackman with the mel *display scale* on STFT data), Mel
   filterbank (2048/hann/128 bands) — or adjust FFT, window, scale,
   resolution, dB range manually.
5. **Output**: file path (auto-suggested next to input).
6. **Generate**: work runs on a background thread; the window stays responsive.
   **Cancel** requests cancellation: the worker stops at the next stage
   boundary, the temp output is removed, a previous valid output is kept,
   and the status shows "Cancelled" (never a false success). Closing the
   window mid-job requests cancellation and defers the close until the
   worker thread is fully done — the window is never destroyed with a
   live worker.
7. **Progress**: bar + stage label (`decode → analyze → render/video → done`).
8. **Result**: PNG preview inline; video shows a saved notice (open externally).
9. **Open output location**: reveals the folder in Explorer.

GPU checkbox enables D3D11 rendering with automatic CPU fallback
(STFT spectrogram only — a Mel run always renders on the CPU).
Reassignment checkbox enables time-frequency reassignment (STFT-only, slower).
