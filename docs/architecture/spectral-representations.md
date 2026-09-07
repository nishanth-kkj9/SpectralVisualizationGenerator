# Spectral Representations (S6.0 architecture)

## Why STFT bins cannot define every representation

Every structure in the codebase assumed `bins == fft_size/2+1` because
every dataset was an STFT dataset. Mel (e.g. 64 bands), Bark, ERB, and
CQT (e.g. 84 bins) define their own counts, centers, and bandwidths —
none derivable from an FFT size. Baking `N/2+1` into shared code would
make every future representation a liar about its own shape.

## Representation vs display scale (the core distinction)

- **Representation** (`RepresentationKind` + `RepresentationInfo`):
  WHAT data was computed — STFT, Mel, Bark, ERB, CQT.
- **Rendering scale** (`FrequencyScale`: linear/log/mel/... in the
  renderers): HOW bins map to pixels.

`STFT data on a logarithmic display` and `Mel data on a linear
display` are different representations. The old Phase-11 plan treated
Mel/Bark/ERB/CQT as display remappings of STFT bins; that model is
superseded for data (it survives only as display mapping until real
filterbanks land).

## Common contract (`src/core/spectral/representation.h`)

`RepresentationInfo`: kind, bins (0 = implied by FFT grid, STFT only),
fmin/fmax (0 = implied full range), bands (filters, or bins-per-octave
for CQT), q, norm (`none`/`slaney`/`area`), phase
(`available`/`n/a` — never fabricate phase), reassignment flag,
explicit `bin_centers` (empty = implied), schema version.

`validate_representation()` is fail-closed: unknown kinds, missing
non-STFT counts, inverted ranges, unordered centers, and bare CQT all
fail. No silent STFT default, no silent Mel default.

## Frame contract (`SpectralFrame`)

Magnitudes are always defined. Phases are meaningful only when the
dataset representation marks phase available. Reassigned coordinates
are meaningful only with representation support AND non-empty vectors.
Frame widths follow the representation (STFT: `N/2+1`; else explicit
bins) — enforced by `validate_dimensions`.

## Identity rules

`analysis_fingerprint()` covers the representation block: STFT→Mel,
64→128 bands, or any Q change moves it; image width or color map does
not; file paths never do. `ProjectConfig` JSON persists the block
(scalars only — bin centers are computed data); dataset JSON persists
it too. The v3 **binary** deliberately excludes it (see below).

## Dataset v4 boundary (decision: YES, later)

Hardened in S6.0-H1 (contracts now enforced, not just described):

- v3 binary datasets are STFT-only. `serialize_binary()` fails closed
  (returns false, no partial bytes) for any non-STFT representation;
  v3 has no version change and no new wire fields.
- Non-STFT dataset identity is unavailable until v4:
  `dataset_identity()` returns empty rather than hashing an incomplete
  picture. STFT identity is unchanged and deterministic.
- `FrequencyAxis` is representation-aware: STFT builds the exact FFT
  grid; non-STFT uses `from_centers()` with explicit per-bin centers.
  Validation requires axis/metadata/frame widths to equal the
  representation bin count (STFT: `fft_size/2+1`).
- Phase is optional per representation: `Available` requires full
  per-frame coverage; `NotApplicable` requires empty vectors (populated
  phase under N/A is rejected — never fabricated).

v4 is required because non-STFT data changes the meaning and shape of
frequency payloads. v4 must add: a representation section to the binary
layout, per-kind bin-count rules in the frame reader (replacing the
current `== nbins` strictness, which is correct only for STFT-shaped
data), and a version bump with v3 rejection preserved. S6.0 designs
the contract only — `SPECTRAL_DATASET_VERSION` stays 3 and no v4
serializer exists. A code note marks this at the version constants.

## What S6.0 deliberately does NOT implement

Mel/Bark/ERB filterbanks, CQT, reassignment or multiband redesigns,
GPU FFT, renderer/video/batch changes. Pipeline output is unchanged:
it still produces conventional STFT datasets, now explicitly tagged
(`kind=stft`, explicit bins, phase available, reassignment per request).

## Remaining FFT-bin assumptions (recorded, not refactored)

These are all correct for STFT data today and must learn the
representation contract when non-STFT data flows:

- `spectrum_renderer` / `spectrogram_renderer` bin→pixel mapping and
  `FreqMapper` treat bins as FFT-grid frequencies.
- `multiband_analyzer` splats onto `max_nfft/2+1` grids.
- `cpu_backend::fft_batch` (`n_fft/2+1` outputs) — FFT backend, stays.
- `stft.h` frame math — STFT by definition, stays.
- Benchmark dataset builders mirror the FFT grid — fixtures, fine.
- `ProjectFreqScale` (rendering scale) still coexists with the new
  representation kinds; renderers keep consuming it until S6 algorithms
  land. The two must not be confused: scale displays, kind defines.
