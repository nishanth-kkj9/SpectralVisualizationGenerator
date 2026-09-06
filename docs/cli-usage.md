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
`--cqt-q`, `--reassigned`, `--multiband`, `--gpu`, `-h`, `-V`.

Exit codes: 0 ok · 1 bad args · 2 file not found · 3 decode error ·
4 analysis error · 5 render error · 6 missing dependency.

Batch prints one `OK`/`FAIL` line per file plus a summary; exit is nonzero
if any file failed. Ctrl-C cancels cleanly. Retries apply per file.
Examples: see `spectragen --help`.
