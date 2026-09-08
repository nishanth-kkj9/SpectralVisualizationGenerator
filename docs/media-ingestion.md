# Media Ingestion (S3)

## Streaming decode

`MediaDecoder` spawns FFmpeg once per file and parses its stdout PCM pipe
in bounded chunks (default 4096 frames). No temporary PCM file, no
whole-file buffer on the decoder side: live state is one chunk plus one
64 KiB pipe buffer regardless of source duration. The pipeline consumes
those chunks through a bounded overlap buffer (`StreamingMonoBuffer`) for
STFT — the only production full-buffer consumer left is the `--multiband`
diagnostic path; see `docs/limitations.md`.

## Tool selection

`ffmpeg`/`ffprobe` are external tools, never bundled. Precedence:

```text
explicit override (set_ffmpeg_path / set_ffprobe_path)
→ FFMPEG_BINARY / FFPROBE_BINARY environment variables
→ bare name on PATH
```

The resolved paths are exposed (`ffmpeg_path()`, `ffprobe_path()`) and
printed by `media_probe`. A missing tool fails `open()` with a diagnostic
naming the tool — never a silent fallback.

## Failure taxonomy

`MediaDecoder::open()` reports a machine-readable `OpenStatus` (not just
bool); the pipeline maps each value to a distinct `JobError`:

```text
missing path          -> FileNotFound      (no subprocess launched)
tool cannot start     -> DependencyMissing  (names ffmpeg/ffprobe)
ffprobe ran, unreadable -> ProbeFailed
valid probe, no audio -> NoAudioStream
ffmpeg cannot start   -> DependencyMissing
ffmpeg exits nonzero  -> DecodeError (failed(), stderr tail kept)
```

Video encoder failures map to `EncodeError` (missing ffmpeg still
`DependencyMissing`); image renderer failures stay `RenderError`.
The CLI keeps its numeric exits: 2/3/4/5 as before, new codes fold in
(`ProbeFailed`/`NoAudioStream` -> 3, `EncodeError` -> 5) except
`DependencyMissing`, which uses the long-reserved exit 6.

## Stream selection

Probe output is parsed as order-independent `flat` key=value (no JSON
dependency, no substring sniffing). Policy: **lowest-index audio stream
wins**; files with no audio stream fail with `media: no audio stream`.
The selected global index, codec, rate, channels, start and duration are
reported by `media_probe`.

## Channels

The decoder exposes the source layout natively — no `-ac` forcing; mono,
stereo, 5.1+ and planar/float sources are all converted by FFmpeg to
interleaved float. Analysis policy (unchanged): the pipeline mixes to
mono with a per-frame mean. `AudioBuffer::mix_down()` implements the same
per-frame mean.

## Timestamps and counts

- `AudioFrame.timestamp`: chunk start, **double** seconds on the stream
  time base (`start_time + delivered / rate`).
- `start_time()`: stream start offset, 0 when unknown.
- Probe counts are **estimates** until a clean EOF: `duration_known()` /
  `total_frames_known()` say which. A nonzero ffmpeg exit sets `failed()`;
  delivered chunks stay valid but the caller must fail the job
  (the pipeline does).

## Resampling

- Decoder: optional `DecodeOptions::target_rate` via libswresample `-ar`
  (band-limited, real). Default 0 = native source rate.
- `AudioBuffer::resample()`: linear interpolation, exact output size
  `round(n * target / source)`, DC-preserving, documented high-frequency
  rolloff. Not used by the pipeline (which decodes at native rate).

## Process safety

All FFmpeg/FFprobe launches go through `SafeProcess`: argv vector quoted
per `CommandLineToArgvW` rules straight into `CreateProcessW` — no shell,
no `_popen`/`system()`. Codec/pixel-format tokens are charset-validated;
`extra_args` is whitespace-split into literal argv elements. stdout/stderr
are captured (stderr drained on a thread so FFmpeg never blocks); real
exit codes are checked at EOF/close and surfaced with stderr tails.

## media_probe

`media_probe <file>` prints input path, selected stream + codec, sample
rate, channels, start, duration (or `unknown`), and resolved tool paths.
Exit 0 on success, 1 with diagnostics on failure, 2 on bad usage.

## Known limits

- The normal pipeline no longer accumulates whole-file audio: decoded
  chunks flow through a bounded overlap buffer (`StreamingMonoBuffer`,
  ~2·fft + one chunk live; measured 24–48 KiB peak for 60 s inputs — see
  `streaming_analysis` in `docs/benchmark-methodology.md`). The
  `SpectralDataset` itself still stores one entry per spectral frame, and
  the `--multiband` diagnostic path remains an explicit full-buffer
  exception (`decode_full_mono`).
- `--ext`-style batch flows are unchanged.
- Video encoding shares `SafeProcess`; codec availability is probed at
  use (unavailable codecs fail `open`, never fake success).
