# Repo structure redesign — spec

Date: 2026-09-06. Status: approved design, pre-implementation.
Goal: professional C++ app layout. No behavior change, no target changes, path swaps only.

## Current (baseline 17cbbd9, branch main, tree clean)

- `src/cli/main.cpp`, `src/gui/*`, `src/core/media/main.cpp` (media_probe stub), `src/benchmarks/*`, `src/core/{audio,batch,dsp,encoding,export,media,pipeline,project,rendering,spectral}/`
- In-src tests: `src/core/dsp/test_fft.cpp`, `src/core/project/test_project_config.cpp`, `src/core/rendering/test_spectrogram_renderer.cpp`, `src/core/rendering/test_spectrum_renderer.cpp`, `src/core/spectral/test_spectral_dataset.cpp`
- `tests/phase{6,7,8,9,10,11,12,13,18,19,20}/`, `tests/accuracy/`; empty `tests/{integration,regression,unit}/`
- `docs/release/` (9 user docs), `docs/superpowers/{plans (7),specs (4)}`, `docs/phase12-reassignment.md`, `docs/phase13-multiband.md`, `docs/{architecture,codebase,licensing}.md`; empty `docs/{dsp,formats,testing}/`
- `dist/*.ps1` tracked, `dist/` tree+zip ignored; empty `scripts/`; root `CMakeLists.txt`, `CMakePresets.json` (no `binaryDir`, hardcoded `E:/` toolchain), `vcpkg.json`, `.github/workflows/ci.yml`
- Known defect (found 2026-09-06): bare `cmake --preset windows-release` configures in-source (root `*.dir/`, `.slnx`, `.qt/`, `*.vcxproj*` leakage). Root cleaned. `.gitignore` lacks `*.dir`, `*.slnx`.

## Target tree

```text
apps/spectragen/main.cpp
apps/spectrascope/{main,main_window,worker}.*
apps/media-probe/main.cpp
src/benchmarks/*                      # unchanged
src/core/{audio,batch,dsp,encoding,export,media,pipeline,project,rendering,spectral}/  # tests moved out, rest unchanged
tests/{dsp,rendering,spectral,video,cli,accuracy,golden}/
tools/packaging/{install,package_portable}.ps1
docs/{accuracy-methodology,analysis-methods,benchmark-methodology,cli-usage,gui-usage,installation,limitations,reproducibility,supported-formats,licensing}.md
docs/{architecture,codebase}/
docs/archive/{plans,specs,phase12-reassignment,phase13-multiband}.md
benchmarks/  # ignored run outputs, unchanged
dist/        # ignored packaging output, scripts moved out
```

## Path map (all via git mv)

| From | To |
|---|---|
| `src/cli/main.cpp` | `apps/spectragen/main.cpp` |
| `src/gui/*` (5) | `apps/spectrascope/` |
| `src/core/media/main.cpp` | `apps/media-probe/main.cpp` |
| `src/core/dsp/test_fft.cpp` | `tests/dsp/test_fft.cpp` |
| `src/core/project/test_project_config.cpp` | `tests/cli/test_project_config.cpp` |
| `src/core/rendering/test_spectrogram_renderer.cpp`, `test_spectrum_renderer.cpp` | `tests/rendering/` |
| `src/core/spectral/test_spectral_dataset.cpp` | `tests/spectral/` |
| `tests/phase9/test_spectragen_cli.cpp` | `tests/cli/` |
| `tests/phase10/test_video_*` (3) | `tests/video/` |
| `tests/phase11/test_frequency_scale.cpp` | `tests/dsp/` |
| `tests/phase11/test_freqscale_render.cpp` | `tests/rendering/` |
| `tests/phase12/test_reassignment.cpp` | `tests/dsp/` |
| `tests/phase12/test_reassignment_render.cpp` | `tests/rendering/` |
| `tests/phase13/test_multiband*.cpp` (2) | `tests/spectral/` |
| `tests/phase18/test_equivalence.cpp` | `tests/cli/` |
| `tests/phase19/test_batch.cpp` | `tests/cli/` |
| `tests/phase20/test_hardening.cpp` | `tests/cli/` |
| `tests/accuracy/test_phase16.cpp` | `tests/spectral/` |
| `tests/accuracy/test_gpu_render.cpp` | `tests/rendering/` |
| `tests/accuracy/accuracy_validator.py`, `ACCURACY_METHODOLOGY.md` | stay `tests/accuracy/` |
| `tests/phase6/golden_*` (4), `tests/phase7/golden_*` (4), `tests/phase8/golden_spectrum_v1.png` | `tests/golden/` |
| `dist/install.ps1`, `dist/package_portable.ps1` | `tools/packaging/` |
| `docs/release/*.md` (9) | `docs/` top level |
| `docs/phase12-reassignment.md`, `docs/phase13-multiband.md` | `docs/archive/` |
| `docs/superpowers/plans/*` (7), `docs/superpowers/specs/*` (4, incl. this file's successor location) | `docs/archive/plans/`, `docs/archive/specs/` |
| Removed (empty, no git content): `scripts/`, `docs/{dsp,formats,testing}/`, `tests/{integration,regression,unit}/` | — |

Empty `tests/phase*` dirs vanish with the moves. Ignored run outputs (`tests/**/_*`, `debug.wav`, `benchmarks/*.json`) untouched.

## Build / CI / ignores

- `CMakeLists.txt`: source-path swaps only. Same targets, same flags, same `NOMINMAX`/`d3d11` links. (Pre-existing C4005 NOMINMAX dup + C4710/C4711 `/Wall` noise out of scope.)
- `CMakePresets.json`: add `"binaryDir": "build"`. Toolchain stays (works locally; CI uses `VCPKG_INSTALLATION_ROOT` + `-B build`, unaffected).
- `.github/workflows/ci.yml`: unchanged (configures `-S . -B build`, no preset use).
- `.gitignore`: add `*.dir`, `*.slnx`; verify with `git check-ignore -v` that bare-preset leakage is covered. Golden exception (`golden_*` tracked) moves with files to `tests/golden/`.
- Ref updates: `docs/installation.md` (`dist/*.ps1` → `tools/packaging/`), `README.md` (`docs/release/*` → `docs/*`).

## Verification (must all pass pre-commit)

1. `cmake -S . -B build` from clean tree leaves root clean (no `*.dir/.slnx/.qt/vcxproj`).
2. Full Release build succeeds.
3. `ctest --test-dir build -C Release`: 21/21 (same count; test NAMES unchanged, only source paths move).
4. `git status`: moves + edits only; no build outputs, no secrets.
5. `actionlint` n/a (no workflow change).

## Rollback

Single commit (`git mv` preserves history). Rollback = revert that commit. If build red after moves: stop, inspect diff, fix paths — no follow-on refactor in same commit.

## Deferred (explicitly out of scope)

- `SpectralCore` INTERFACE → compiled lib split.
- Docs content rewrite (archive only).
- Bench runner move (stays `src/benchmarks`).
- `AGENTS.md` wrong `npm run build` line (needs owner call).
- `CMakePresets.json` hardcoded `E:/` toolchain → `$env{VCPKG_ROOT}` portability.
