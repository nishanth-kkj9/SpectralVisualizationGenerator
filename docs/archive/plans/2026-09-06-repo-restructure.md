# Repo restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move every file to the approved standard layout with zero behavior change.

**Architecture:** Pure `git mv` relocation + path-string swaps. Headers stay put, so all quoted flat includes keep resolving via unchanged per-target include dirs. No target, flag, or test-name changes.

**Tech Stack:** CMake 4.x, MSVC 19.51 (VS18 2026), vcpkg manifest, PowerShell.

**Spec:** `docs/superpowers/specs/2026-09-06-repo-structure-design.md`

## Global Constraints

- One single commit for the whole restructure (spec rollback requirement). No per-task commits.
- ctest NAMES unchanged: 21/21 must pass with identical test names.
- `golden_*` fixtures stay tracked; `tests/**/_*` + `debug.wav` stay ignored.
- Verified evidence (not guesses): all test .cpps use quoted flat includes; golden paths are WRITE paths compared only against nonexistent `.expected.png`; ps1 scripts derive `$root` from `$PSScriptRoot`; `accuracy_validator.py:78` points at `src/core` (unchanged); no test references `debug.wav`; `installation.md` needs no edit (only zip-name mention); `RELEASE_NOTES_v0.1.0.md` is history, do not touch.

---

### Task 1: apps/ + in-src tests out of src/

**Files:**
- Move: `src/cli/main.cpp` → `apps/spectragen/main.cpp`
- Move: `src/gui/main.cpp`, `src/gui/main_window.cpp`, `src/gui/main_window.h`, `src/gui/worker.cpp`, `src/gui/worker.h` → `apps/spectrascope/`
- Move: `src/core/media/main.cpp` → `apps/media-probe/main.cpp`
- Move: `src/core/dsp/test_fft.cpp` → `tests/dsp/test_fft.cpp`
- Move: `src/core/project/test_project_config.cpp` → `tests/cli/test_project_config.cpp`
- Move: `src/core/rendering/test_spectrogram_renderer.cpp`, `src/core/rendering/test_spectrum_renderer.cpp` → `tests/rendering/`
- Move: `src/core/spectral/test_spectral_dataset.cpp` → `tests/spectral/test_spectral_dataset.cpp`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: new app/test paths consumed by Task 4 (CMakeLists swaps).

- [ ] **Step 1: git mv the app entry points**

```bash
git mv src/cli/main.cpp apps/spectragen/main.cpp
git mv src/gui/main.cpp src/gui/main_window.cpp src/gui/main_window.h src/gui/worker.cpp src/gui/worker.h apps/spectrascope/
git mv src/core/media/main.cpp apps/media-probe/main.cpp
```

- [ ] **Step 2: git mv the in-src tests**

```bash
git mv src/core/dsp/test_fft.cpp tests/dsp/test_fft.cpp
git mv src/core/project/test_project_config.cpp tests/cli/test_project_config.cpp
git mv src/core/rendering/test_spectrogram_renderer.cpp src/core/rendering/test_spectrum_renderer.cpp tests/rendering/
git mv src/core/spectral/test_spectral_dataset.cpp tests/spectral/test_spectral_dataset.cpp
```

- [ ] **Step 3: Verify this task**

Run: `git status --porcelain`
Expected: 12 renames (`R`), nothing else. `src/cli/`, `src/gui/` gone.

### Task 2: tests/ flatten to feature dirs + golden path edits

**Files:**
- Move: `tests/phase9/test_spectragen_cli.cpp` → `tests/cli/`
- Move: `tests/phase10/test_video_encoder.cpp`, `test_video_renderer.cpp`, `test_video_cli.cpp` → `tests/video/`
- Move: `tests/phase11/test_frequency_scale.cpp` → `tests/dsp/`; `test_freqscale_render.cpp` → `tests/rendering/`
- Move: `tests/phase12/test_reassignment.cpp` → `tests/dsp/`; `test_reassignment_render.cpp` → `tests/rendering/`
- Move: `tests/phase13/test_multiband.cpp`, `test_multiband_compare.cpp` → `tests/spectral/`
- Move: `tests/phase18/test_equivalence.cpp`, `tests/phase19/test_batch.cpp`, `tests/phase20/test_hardening.cpp` → `tests/cli/`
- Move: `tests/accuracy/test_phase16.cpp` → `tests/spectral/`; `tests/accuracy/test_gpu_render.cpp` → `tests/rendering/`
- Move: 9 goldens (`tests/phase6/golden_*.png` ×4, `tests/phase7/golden_*.png` ×4, `tests/phase8/golden_spectrum_v1.png`) → `tests/golden/`
- Modify: `tests/rendering/test_spectrogram_renderer.cpp:378-379` (`tests/phase6` → `tests/golden`, both `golden_dir` and `create_directories`)
- Modify: `tests/rendering/test_spectrum_renderer.cpp:500,525` (`tests/phase7` → `tests/golden`, both `create_directories` and `path`)
- Modify: `tests/cli/test_project_config.cpp:473,483` (`tests/phase8` → `tests/golden`, both `create_directories` and `png_path`)

**Interfaces:**
- Consumes: Task 1 paths.
- Produces: final test paths consumed by Task 4.

- [ ] **Step 1: git mv phase tests to feature dirs**

```bash
git mv tests/phase9/test_spectragen_cli.cpp tests/cli/
git mv tests/phase10/test_video_encoder.cpp tests/phase10/test_video_renderer.cpp tests/phase10/test_video_cli.cpp tests/video/
git mv tests/phase11/test_frequency_scale.cpp tests/dsp/
git mv tests/phase11/test_freqscale_render.cpp tests/rendering/
git mv tests/phase12/test_reassignment.cpp tests/dsp/
git mv tests/phase12/test_reassignment_render.cpp tests/rendering/
git mv tests/phase13/test_multiband.cpp tests/phase13/test_multiband_compare.cpp tests/spectral/
git mv tests/phase18/test_equivalence.cpp tests/phase19/test_batch.cpp tests/phase20/test_hardening.cpp tests/cli/
git mv tests/accuracy/test_phase16.cpp tests/spectral/
git mv tests/accuracy/test_gpu_render.cpp tests/rendering/
```

- [ ] **Step 2: git mv the 9 goldens**

```bash
git mv tests/phase6/golden_heat_lin.png tests/phase6/golden_heat_log.png tests/phase6/golden_viridis_lin.png tests/phase6/golden_viridis_log.png tests/golden/
git mv tests/phase7/golden_lin_heat.png tests/phase7/golden_lin_viridis.png tests/phase7/golden_log_heat.png tests/phase7/golden_log_viridis.png tests/golden/
git mv tests/phase8/golden_spectrum_v1.png tests/golden/
```

- [ ] **Step 3: Edit the 3 golden write-paths (exact strings verified by read)**

In `tests/rendering/test_spectrogram_renderer.cpp`: `const std::string golden_dir = "tests/phase6";` → `"tests/golden"`.
In `tests/rendering/test_spectrum_renderer.cpp`: `fs::create_directories("tests/phase7");` → `"tests/golden"`; `std::string("tests/phase7/golden_")` → `std::string("tests/golden/golden_")`.
In `tests/cli/test_project_config.cpp`: `fs::create_directories("tests/phase8");` → `"tests/golden"`; `"tests/phase8/golden_spectrum_v1.png"` → `"tests/golden/golden_spectrum_v1.png"`.

- [ ] **Step 4: Verify this task**

Run: `git status --porcelain | Select-String "^R" | Measure-Object | Select-Object -ExpandProperty Count`
Expected: cumulative renames only, plus 3 `M` test sources. No other modifications.

### Task 3: tools/, docs/archive/, dead-dir removal

**Files:**
- Move: `dist/install.ps1`, `dist/package_portable.ps1` → `tools/packaging/`
- Move: `docs/release/*.md` (9) → `docs/`
- Move: `docs/phase12-reassignment.md`, `docs/phase13-multiband.md` → `docs/archive/`
- Move: `docs/superpowers/plans/*` (7 + this plan) → `docs/archive/plans/`; `docs/superpowers/specs/*` (5) → `docs/archive/specs/`
- Delete dirs (all verified empty, no tracked content): `scripts/`, `docs/dsp/`, `docs/formats/`, `docs/testing/`, `tests/integration/`, `tests/regression/`, `tests/unit/`, emptied `tests/phase*/`, `src/cli/`, `src/gui/`
- Delete from disk (ignored regenerable residue, NOT via git): `tests/phase10/debug.wav`, `tests/phase7/_render_to_png.png`, `tests/phase7/_roundtrip.png`, `tests/phase8/_det_a.png`, `tests/phase8/_det_b.png`, `tests/phase8/_round_trip.json`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces: final docs/tools paths consumed by Task 4 link edits.

- [ ] **Step 1: git mv tools + docs**

```bash
git mv dist/install.ps1 dist/package_portable.ps1 tools/packaging/
git mv docs/release/accuracy-methodology.md docs/release/analysis-methods.md docs/release/benchmark-methodology.md docs/release/cli-usage.md docs/release/gui-usage.md docs/release/installation.md docs/release/limitations.md docs/release/reproducibility.md docs/release/supported-formats.md docs/
git mv docs/phase12-reassignment.md docs/phase13-multiband.md docs/archive/
git mv docs/superpowers/plans docs/archive/plans
git mv docs/superpowers/specs docs/archive/specs
```

- [ ] **Step 2: Remove dead dirs + ignored residue**

```bash
Remove-Item -Recurse -Force scripts, docs/dsp, docs/formats, docs/testing, tests/integration, tests/regression, tests/unit
Remove-Item -Recurse -Force tests/phase6, tests/phase7, tests/phase8, tests/phase9, tests/phase10, tests/phase11, tests/phase12, tests/phase13, tests/phase18, tests/phase19, tests/phase20, src/cli, src/gui, docs/release, docs/superpowers
```

- [ ] **Step 3: Verify this task**

Run: `git status --porcelain | Select-String "^\?\?"`
Expected: empty (no untracked leftovers; residue files were ignored so their deletion shows nothing).

### Task 4: Path-string swaps (CMakeLists, presets, gitignore, README, ps1)

**Files:**
- Modify: `CMakeLists.txt` — exact swaps: `src/cli/main.cpp`→`apps/spectragen/main.cpp`; `src/gui/main.cpp|main_window.cpp|worker.cpp`→`apps/spectrascope/...`; `tests/phase9/test_spectragen_cli.cpp`→`tests/cli/...`; `src/core/media/main.cpp`→`apps/media-probe/main.cpp`; `src/core/dsp/test_fft.cpp`→`tests/dsp/test_fft.cpp` (3 occurrences: test_fft, test_fft_asan, test_fft_msan, test_fft_asan_msvc targets — verify count with Select-String, replace all); `src/core/spectral/test_spectral_dataset.cpp`→`tests/spectral/...`; `src/core/rendering/test_spectrogram_renderer.cpp`→`tests/rendering/...`; `src/core/rendering/test_spectrum_renderer.cpp`→`tests/rendering/...`; `src/core/project/test_project_config.cpp`→`tests/cli/...`; `tests/phase10/test_video_{encoder,renderer,cli}.cpp`→`tests/video/...`; `tests/phase11/test_frequency_scale.cpp`→`tests/dsp/...`; `tests/phase11/test_freqscale_render.cpp`→`tests/rendering/...`; `tests/phase12/test_reassignment.cpp`→`tests/dsp/...`; `tests/phase12/test_reassignment_render.cpp`→`tests/rendering/...`; `tests/phase13/test_multiband*.cpp`→`tests/spectral/...`; `tests/accuracy/test_phase16.cpp`→`tests/spectral/...`; `tests/accuracy/test_gpu_render.cpp`→`tests/rendering/...`; `tests/phase19/test_batch.cpp`→`tests/cli/...`; `tests/phase20/test_hardening.cpp`→`tests/cli/...`; `tests/phase18/test_equivalence.cpp`→`tests/cli/...`. Include dirs untouched.
- Modify: `CMakePresets.json` — add `"binaryDir": "build"` to both presets.
- Modify: `.gitignore` — add `*.dir` and `*.slnx` (in-source configure leakage found 2026-09-06).
- Modify: `README.md` — 4 refs: `` `docs/release/cli-usage.md` ``→`` `docs/cli-usage.md` ``; `` `docs/release/gui-usage.md` ``→`` `docs/gui-usage.md` ``; `` `dist/package_portable.ps1` ``→`` `tools/packaging/package_portable.ps1` ``; `` `docs/release/` ``→`` `docs/` ``.
- Modify: `tools/packaging/package_portable.ps1` + `tools/packaging/install.ps1` — `$root = Split-Path -Parent $PSScriptRoot` → `$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)`; usage comment `dist/package_portable.ps1` → `tools/packaging/package_portable.ps1` (same for install).

**Interfaces:**
- Consumes: Tasks 1–3 final paths.
- Produces: buildable tree for Task 5.

- [ ] **Step 1: Swap CMakeLists source paths (mechanical, one old→new each)**

Run after edits: `Select-String -Path CMakeLists.txt -Pattern "src/cli|src/gui|phase[0-9]|tests/accuracy/test_|src/core/.*/test_" | Measure-Object`
Expected: 0 matches (every old path gone).

- [ ] **Step 2: Presets + gitignore + README + ps1 edits as listed**
- [ ] **Step 3: Verify this task**

Run: `git diff --stat`
Expected: `CMakeLists.txt | ~40 +-`, `CMakePresets.json | 2 +`, `.gitignore | 2 +`, `README.md | 4 +-`, 2 ps1 small diffs, 3 test-source golden diffs. Nothing else.

### Task 5: Verify (gates — all must pass)

- [ ] **Step 1: Clean-tree configure (proves preset fix + no root pollution)**

```bash
Remove-Item -Recurse -Force build/* -ErrorAction SilentlyContinue
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 "-DCMAKE_TOOLCHAIN_FILE=E:/program_files/vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
git status --porcelain | Select-String "^\?\?"
```

Expected: configure succeeds; no new `??` entries at root (no `*.dir/.slnx/.qt/vcxproj`).

- [ ] **Step 2: Full Release build**

```bash
cmake --build build --config Release --parallel
```

Expected: success, warnings only (pre-existing C4710/C4711/C4005).

- [ ] **Step 3: ctest**

```bash
ctest --test-dir build -C Release --output-on-failure | Select-Object -Last 25
```

Expected: `100% tests passed out of 21` with identical test names.

### Task 6: Commit + push (single commit per spec)

- [ ] **Step 1: Pre-commit identity gate**

```bash
git config --global --get user.name; git config --global --get user.email
git var GIT_AUTHOR_IDENT; git var GIT_COMMITTER_IDENT
```

Expected: `Nishanth`, `nishanthkkj@gmail.com`, idents match.

- [ ] **Step 2: Stage everything + commit + verify + push**

```bash
git add -A
git status --porcelain
git commit -m "refactor(repo): standard apps-src-tests-tools-docs layout"
git log -1 --format="%an <%ae> %cn <%ce>"
git push origin main
```

Expected: one commit, ident `Nishanth <nishanthkkj@gmail.com>` ×2, push `main -> main`, clean tree.
