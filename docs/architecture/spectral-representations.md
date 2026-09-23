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
it too, **including `bin_centers`** (so explicit non-STFT centers
round-trip) plus `analysis_method` and per-frame `band_count`.
The v3 **binary** deliberately excludes the representation section (see
below); on load the STFT default is restated from the payload itself
(bin count from the axis, reassignment support from array presence —
v3 writers emit those if and only if enabled).

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

## What S6.0 deliberately did NOT implement (Mel landed in Phase 9)

S6.0/H1/Phase 8 designed and enforced the contracts only; Phase 9 then added
the first real filterbank representation (Mel — see the next section).
Still absent: Bark/ERB filterbanks, CQT, reassignment or multiband
redesigns, GPU FFT, and representation-aware GPU rendering. STFT output is
unchanged by Phase 9 (`kind=stft`, explicit bins, phase available,
reassignment per request).

## Phase 8 enforcement (contracts, not just described)

On top of S6.0-H1, the following are now enforced:

- **Validation**: STFT ranges must match the actual
  axis ends exactly (the range *is* the kept center span); for filterbank
  kinds the range is *coverage* — first left edge .. last right edge — so
  the actual centers must lie INSIDE it. STFT with an implied range
  requires the full grid, with an
  explicit range (slice/decimation) every center must lie on the FFT
  grid; phase normalization without phase capability fails; multiband
  output (phaseless by construction) is tagged phase N/A.
- **Transforms**: `filter_band` / `downsample_frequency` retarget axis,
  analysis bin count, representation bins/range/centers (STFT counts go
  back to implied; STFT nyquist/resolution untouched, explicit axes
  refresh nominal values), slice reassigned arrays, and refuse invalid
  input instead of reading out of bounds. Phase-less data stays
  phase-less; `export_csv` leaves the phase column empty rather than
  fabricating values.
- **Renderers are STFT-only by contract**: spectrogram, spectrum, video,
  and GPU paths return `UnsupportedRepresentation` for anything else
  instead of drawing FFT pictures of filterbank data (mapped to
  `RenderError`/`EncodeError` with the kind named).
- **Serialization**: binary resets stale representation state on load
  (v3 is STFT-only); JSON resets then parses, round-tripping kind,
  bins, range, bands, q, norm, phase, reassignment, version,
  bin_centers, method, and frame band counts.

## Mel representation (Phase 9 — implemented)

The first real non-STFT representation, computed by `MelFilterbank`
(`src/core/dsp/mel_filterbank.h`) inside the existing streaming analysis
loop. **Mel is not a display scale**: STFT spectral energy is actually
aggregated through triangular filters into explicit bands. Mel data on a
logarithmic display is still Mel data, and `--freq-scale mel` on STFT data
is still STFT data.

- **Frequency conversion** (project formula — the display mapper uses the
  same scale definition, which is why the helper is shared):
  `mel(f) = 2595*log10(1 + f/700)`, `hz(m) = 700*(10^(m/2595) - 1)`,
  `mel(0) = 0`. Monotonic and invertible: a tone moving up in Hz always
  moves to a higher (or equal) band.
- **Filterbank construction**: `bands` triangles whose centers are uniformly
  spaced in Mel between `mel(fmin)` and `mel(fmax)`; band `b` spans
  `[edge_b, edge_b+2]`, with the first left edge = `fmin` and the last right
  edge = `fmax` (the configured range is edge *coverage*, which is why
  validation demands containment of the centers rather than equality).
  Weights interpolate linearly in Hz over the uniform FFT-bin grid. `build()`
  fails with a typed `BadConfig` (before any data is produced) on: sample
  rate <= 0, FFT size < 2 or not a power of two, `fmax > Nyquist`,
  `fmin >= fmax`, `bands <= 0`, `bands >` one-sided FFT bins, unknown
  normalization, degenerate Mel range, violated `left < center < right`, or a
  zero-area/zero-weight filter (bands too dense for the FFT resolution).
  Nothing silently falls back to STFT.
- **Normalization (`RepresentationNorm`, three DISTINCT operations)**
  - `None` — raw peak-1 triangles.
  - `Slaney` — each filter divided by half its Hz width `(right-left)/2`,
    i.e. its analytic triangle area: unit area in Hz.
  - `Area` — each filter divided by its discrete weight sum (unit sum).
- **Aggregation input (one authoritative definition)**: the one-sided
  **amplitude-corrected power** of the STFT contract — interior bins
  doubled, DC/Nyquist single, scale `1/(N*cg)` — the identical quantity the
  STFT representation stores in `frame.power[k]`. A Mel frame's energy is
  weighted power accumulation `E(b) = sum_k H_b(k)*P(k)` and
  `frame.magnitudes[b] = sqrt(E(b))`, so `power == mag^2` holds in every
  representation and Mel magnitudes live in the same amplitude domain as
  STFT magnitudes (shared `reference_amplitude`/`db_floor` rendering
  semantics apply unchanged). Aggregating raw `|X|^2` would carry the
  window's `N*cg` factor (~+55 dB at N=1024/Hann) while claiming neutral
  window gains — the dedicated amplitude-domain test fails loudly on that.
- **Metadata**: `kind = mel`, `bins = bands` (never `N/2+1`),
  `bin_centers` = the actual Mel centers in Hz (authoritative), `fmin_hz` /
  `fmax_hz` = first left edge / last right edge, `bands = bands`, `norm`,
  `phase = n/a`, `reassignment_supported = false`. `fft_size`/`hop_size`
  stay the underlying STFT implementation parameters (frames keep
  `n_fft = fft_size`), `analysis_metadata.num_frequency_bins` is the band
  count, `analyzer_version = "1"`, `analysis_method = "mel"`.
  `normalization_info.window_*gain` is neutral because the coherent gain is
  already divided out of the aggregated power.
- **Statistics** use the stored band magnitudes and the real Mel centers
  (never `k*sr/N`): `rms = sqrt(mean_b mag_b^2)`,
  `peak_magnitude = max_b mag_b`,
  `spectral_centroid = sum_b f_b*mag_b / sum_b mag_b`,
  `spectral_bandwidth = sqrt(sum_b (f_b - centroid)^2*mag_b / sum_b mag_b)`.
- **Phase and reassignment**: Mel is phaseless — frames carry empty
  `phases` and empty reassignment vectors, and validation rejects populated
  phase under `n/a`. STFT reassignment coordinates are never mapped onto
  bands.
- **Frequency axis**: `FrequencyAxis::from_centers(centers, sr)` — explicit
  and nonuniform in Hz. The axis centers are authoritative; the nominal
  spacing is descriptive only and is never treated as FFT resolution.
- **Rendering**: `MelSpectrogramRenderer`
  (`src/core/rendering/mel_renderer.cpp`) maps the explicit centers to image
  rows and frames to columns, using the same `normalize_db`/`color_map`
  helpers and dB convention (`20*log10(mag/ref)`) as the STFT renderer. It
  refuses STFT and every other kind. `SpectrogramRenderer` stays STFT-only
  and never guesses a kind; `VideoRenderer` and the pipeline's image path
  dispatch on the dataset kind, so a Mel run renders through the Mel
  renderer and an STFT run is untouched.
- **GPU**: unchanged and STFT-only (`gpu_spectrogram` refuses non-STFT).
  Mel always renders on the CPU, so `--gpu` has no effect on a Mel run. No
  GPU Mel compute exists and none is claimed.
- **Serialization**: JSON round-trips the whole Mel contract (kind, bins,
  fmin/fmax, bands, norm, phase, reassignment, version, `bin_centers`,
  `analysis_method`, per-frame band counts) and loads back as Mel, not STFT.
  The v3 **binary** stays STFT-only by contract (fails closed).
- **Identity**: `analysis_fingerprint()` hashes the whole `analysis` block,
  so representation kind, band count, normalization, frequency range and
  version all move it, while renderer/notes/metrics do not.
- **Streaming and memory**: Mel shares the STFT framing path — bounded
  decoder chunks -> overlap buffer -> one frame -> filterbank -> dataset — so
  results are independent of decoder chunk boundaries, the filterbank is
  built once per analysis (never per frame), and the working set scales with
  FFT size + band count, not with source duration.

## Remaining FFT-bin assumptions (recorded, not refactored)

These are all correct for STFT data today and must learn the
representation contract when non-STFT data flows:

- `multiband_analyzer` splats onto `max_nfft/2+1` grids.
- `cpu_backend::fft_batch` (`n_fft/2+1` outputs) — FFT backend, stays.
- `stft.h` frame math — STFT by definition, stays.
- Benchmark dataset builders mirror the FFT grid — fixtures, fine.
- `ProjectFreqScale` (rendering scale) still coexists with the new
  representation kinds; renderers keep consuming it until S6 algorithms
  land. The two must not be confused: scale displays, kind defines.
