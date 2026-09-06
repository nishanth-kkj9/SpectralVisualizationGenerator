# Reproducibility (S5)

Four distinct identities — do not collapse them into one vague fingerprint:

- **Input identity**: SHA-256 of the source *bytes* (`DecodedMedia.file_hash`,
  hashed once per job). Same bytes under any path hash identically; the path
  itself is informational only.
- **Analysis fingerprint** (`ProjectConfig::analysis_fingerprint`): input
  content identity + every result-affecting analysis field (FFT, hop,
  window, gains, rate, channels, method, algorithm version). Informational
  fields (`notes`, `project_id`, `created_utc`, renderer, software, tier)
  do not move it.
- **Dataset identity** (`SpectralDataset::dataset_identity`): SHA-256 over
  the canonical v3 binary bytes. Same semantic content ⇒ same bytes ⇒
  same identity (no timestamps or locale data in the artifact). The FNV-1a
  header checksum is corruption detection only, not identity.
- **Render fingerprint** (`ProjectConfig::render_fingerprint`): dataset
  identity + render-affecting parameters (renderer, frequency range,
  dynamic range).

## Guarantees (each pinned by tests)

- Same binary + same input + same configuration ⇒ **byte-identical
  dataset bytes** (`test_reproducibility`: analyze twice, identical bytes
  and identity; save → load → render twice, identical PNGs).
- CLI and GUI call the same `run_job` pipeline; equivalent configurations
  produce equivalent analytical results by construction.
- Outputs are written atomically (temp file + rename): reruns and overwrites
  are idempotent, interrupted jobs leave no partial files.
- Batch mirrors the input tree under the output dir, so each file's result is
  independently verifiable.

## Explicitly not promised

- Video bytes are not byte-identical across runs (lossy codecs); the config
  records encoder parameters for *similar* re-encodes.
- `ProjectConfig::fingerprint()` (whole-document SHA-256) exists for
  backward compatibility; it moves with informational fields and is NOT
  the analytical identity — use `analysis_fingerprint()` for that.

## Binary format (v3)

Magic `SPDT` (u32 `0x53504454`), version 3, explicit little-endian
fixed-width integers, IEEE-754 floats, length-prefixed strings, FNV-1a-64
header checksum, 4 GB hard cap with per-field bounds (strings ≤ 4 MB,
frames ≤ 16 M, bins ≤ 1 M). Only version 3 loads; v1/v2 are rejected.
See `docs/media-ingestion.md` for the decode side.
