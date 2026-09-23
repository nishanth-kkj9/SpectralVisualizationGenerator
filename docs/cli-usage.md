# CLI Usage — `spectragen`

```
spectragen <input> --output <output.png> [options]
spectragen batch <input> --output <dir> [options] [--recursive] [--jobs N] [--retries N] [--ext a,b]
```

Key options: `-v spectrogram|spectrum`, `--fft` (pow2), `--hop`, `--window
hann|hamming|blackman|rectangular`, `--overlap`, `--min-frequency`,
`--max-frequency`, `--db-range`, `--resolution WxH`, `--output-format
image|video` (auto from extension), `--fps`, `--codec`, `--crf`,
`--duration`, `--freq-scale linear|log|mel|bark|erb|cqt`, `--cqt-center`,
`--cqt-q`, `--representation stft|mel`, `--mel-bands`, `--mel-norm`,
`--reassigned`, `--multiband`, `--gpu`, `-h`, `-V`.

`--representation` selects WHAT is computed: `stft` (FFT bins, default) or
`mel` (real triangular Mel filterbank bands). `--mel-bands <n>` (1..fft-bins,
default 64) and `--mel-norm none|slaney|area` (default `slaney`) configure the
filterbank and are rejected outside a Mel run. `--freq-scale` is purely
display mapping and never changes the representation. `--reassigned` is
STFT-only and fails when combined with `--representation mel`; Mel data is
phaseless by contract. `--gpu` covers the STFT spectrogram renderer only —
a Mel run renders on the CPU with identical geometry. Mel datasets persist
through JSON (`--output-format` videos render through the Mel renderer); the
dataset binary format stays STFT-only.

Exit codes: 0 ok · 1 bad args · 2 file not found · 3 decode error ·
4 analysis error · 5 render error · 6 missing dependency · 7 cancelled.

Ctrl-C / Ctrl-Break requests cancellation: decode, analysis, rendering,
and encoding stop at the next stage boundary (pipe polls ~5ms, otherwise
per chunk/frame/row), the ffmpeg child is terminated, the temp output is
removed, and any previous valid output is kept. The process exits 7.
Probe (ffprobe) and an in-flight GPU dispatch are short bounded steps
that finish first; cancellation is prompt, not instant.

All numeric options are parsed strictly: empty strings, whitespace,
hex (`0x..`), trailing characters (`1024abc`), and NaN/infinity are
rejected. `--hop` is authoritative; `--overlap` sets
hop=round(fft·(1−overlap)) and must agree with `--hop` if both are given.
`--resolution` is capped at 32768 per side and 268M pixels total.

Output format vs extension: image output is PNG-only, so `--output`
must end in `.png` (case-insensitive) or have no extension; video output
requires `.mp4`, `.webm`, or `.mkv` (ffmpeg sniffs the container from the
extension). Without `--output-format`, a video extension infers video and
anything else infers image; an explicit `--output-format` that contradicts
the extension fails instead of silently mislabeling the file. New bytes go
to a uniquely named temp file beside the destination and replace it on
success; a failed job never leaves a partial file and never deletes a
previous valid output.

Batch prints one `OK`/`FAIL` line per file plus a summary; exit is nonzero
if any file failed. Ctrl-C cancels cleanly. Retries apply per file.
Examples: see `spectragen --help`.
