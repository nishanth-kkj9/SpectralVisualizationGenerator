# GUI Usage — SpectraScope

Thin client over the same pipeline as the CLI. No editor/player features.

1. **Select/drop** audio or video (Browse or drag onto the window).
2. **Visualization**: Spectrogram or Spectrum; output format Image (PNG) or Video (MP4).
   The format combo always decides: a typed output path whose extension
   contradicts it (e.g. Image + `.mp4`) is rejected before the job starts.
3. **Preset**: Voice (1024/hann), Music (2048/hann), Detail (4096/blackman/mel) —
   or adjust FFT, window, scale, resolution, dB range manually.
4. **Output**: file path (auto-suggested next to input).
5. **Generate**: work runs on a background thread; the window stays responsive.
   **Cancel** requests cancellation: the worker stops at the next stage
   boundary, the temp output is removed, a previous valid output is kept,
   and the status shows "Cancelled" (never a false success). Closing the
   window mid-job requests cancellation and defers the close until the
   worker thread is fully done — the window is never destroyed with a
   live worker.
6. **Progress**: bar + stage label (`decode → analyze → render/video → done`).
7. **Result**: PNG preview inline; video shows a saved notice (open externally).
8. **Open output location**: reveals the folder in Explorer.

GPU checkbox enables D3D11 rendering with automatic CPU fallback.
Reassignment checkbox enables time-frequency reassignment (slower).
