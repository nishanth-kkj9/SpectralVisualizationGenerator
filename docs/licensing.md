# Dependency and License Audit — Phase 21

Date: 2026-09-06. Sources: `vcpkg list`, `vcpkg_installed/<triplet>/share/*/copyright`,
`ffmpeg -version` / `-buildconf`, `dumpbin /dependents`, source inspection.
No vendored third-party source code was found in `src/` (searched for
stb_*, miniz, kissfft, dr_*, nanosvg, bundled LICENSE/COPYING files — none).

> This document records facts from available license texts. It makes no legal
> conclusions. Items needing professional legal review are marked **[LEGAL]**.

## How dependencies reach the binaries

| Binary | Third-party linkage (verified via `dumpbin /dependents`) |
|--------|----------------------------------------------------------|
| `spectra_gui.exe` | Qt6Core/Gui/Widgets **DLLs** (dynamic), d3d11.dll + D3DCOMPILER_47.dll (Windows system), MSVC CRT DLLs (dynamic `/MD`) |
| `spectragen.exe`, tests, benchmarks | MSVC CRT DLLs only. **No** FFmpeg linkage, **no** Qt linkage |
| FFmpeg | External **subprocess** (`ffmpeg`/`ffprobe` on PATH). Never linked, never embedded |

Triplet: `x64-windows` (dynamic). Manifest: `vcpkg.json` (`qtbase` with `gui`, `widgets`).
Linkage below is per-`dumpbin /dependents` on `spectra_gui.exe` (direct: Qt6 DLLs,
d3d11, D3DCOMPILER_47, MSVC CRT) plus the transitive closure those Qt DLLs load.
No static third-party linkage anywhere; no FFmpeg or Qt linkage in `spectragen`.

## Inventory

| # | Name | Version | License (per shipped copyright file) | Static/Dynamic | Redistribution notes (from license text) | Attribution | Build options relevant |
|---|------|---------|--------------------------------------|----------------|------------------------------------------|-------------|------------------------|
| 1 | qtbase (Core, Gui, Widgets, Network, Sql, Test, Concurrent) | 6.11.1 | Qt Company tri-license: Commercial / GPL-2.0 / GPL-3.0 / LGPL-3.0. vcpkg `share/qtbase/copyright` contains AFL-2.1 text (covers D-Bus components); full Qt license texts are **not** shipped in the vcpkg tree | Dynamic (Qt6*.dll in `vcpkg_installed/x64-windows/bin`) | **[LEGAL]** Under LGPL-3.0 (if chosen): ship license copy, preserve notices, allow relinking/replacement of the Qt DLLs, provide corresponding source offer for Qt itself | Qt Company + The Qt Company Ltd copyright notices | `qtbase[gui,widgets,...]` features; see manifest |
| 2 | FFmpeg (gyan full build, external binary) | 8.1.2 | GPL-3.0 (`--enable-gpl --enable-version3` in `-buildconf`; also bundles GPL libs x264/x265 etc.) | Not linked (subprocess) | **[LEGAL]** Do **not** redistribute this binary with the app. It is a user-supplied external tool. If FFmpeg is ever bundled or linked, GPL-3.0 applies to the combined work | FFmpeg developers (printed by `ffmpeg -version`) | External; path via `FFMPEG_BINARY`/`FFPROBE_BINARY` env or PATH |
| 3 | FFT backend (`src/core/dsp/fft.h`) | n/a (own code) | None — hand-written radix-2 Cooley-Tukey | Source-compiled | None | n/a | n/a |
| 4 | GPU backend | n/a | D3D11/D3DCompiler are Windows OS components (Windows SDK license, no redistribution — target machines provide them). HLSL shaders are inline own code. VkFFT/GPU-FFT is a **stub only** (`gpu_backend.h` throws) | System DLLs, dynamic | None beyond Windows itself | n/a | `NOMINMAX`, `d3d11.lib d3dcompiler.lib` |
| 5 | PNG codec (`png_encoder.cpp`) | n/a (own code) | None — hand-written encoder, DEFLATE stored blocks | Source-compiled | None | n/a | n/a |
| 6 | Viridis colormap LUT | matplotlib data | Public domain (per comment citing van der Walt & Smith 2015) | Embedded data | None required; attribution retained in source comment | Stated in `spectrogram_renderer.cpp` + `gpu_spectrogram.cpp` | n/a |
| 7 | VideoRenderer | n/a (own code) | None — shells external ffmpeg (see #2) | Subprocess | Same as #2 | n/a | Codec/crf passed to ffmpeg CLI |
| 8 | OpenSSL | 3.6.3 | Apache-2.0 (copyright file header) | Dynamic via Qt Network stack (pulled by `qtbase[openssl]`, `libpq[openssl]`) | Preserve NOTICE/license copy per Apache-2.0 text | OpenSSL project | vcpkg default |
| 9 | ICU | 78.3 | Unicode-3.0 (`UNICODE LICENSE V3`) | Dynamic via Qt | Preserve copyright + permission notice | Unicode, Inc. | `icu[tools]` |
| 10 | FreeType | 2.14.3 | FTL/GPL dual (`FREETYPE LICENSES`) | Dynamic via Qt font engine | FTL: preserve license file + no misleading endorsement | FreeType authors | features: brotli, bzip2, png, zlib |
| 11 | HarfBuzz | 14.3.1 | Old MIT ("MIT" per COPYING) | Dynamic via Qt | Preserve copyright + permission notice | HarfBuzz authors | `harfbuzz[freetype]` |
| 12 | libpng | 1.6.58 | libpng license (permissive, see COPYRIGHT section) | Dynamic via Qt/freetype | Preserve copyright notice + disclaimer | libpng authors (see file) | vcpkg default |
| 13 | libjpeg-turbo | 3.2.0 | BSD-style (see "libjpeg-turbo Licenses") | Dynamic via Qt | Preserve copyright + license terms | IJG + libjpeg-turbo authors | vcpkg default |
| 14 | PCRE2 | 10.47 | BSD (see LICENCE) | Dynamic via QtCore | Preserve copyright + license | PCRE2 / University of Cambridge | `pcre2[jit]`, default features |
| 15 | SQLite | 3.53.4 | Public domain | Dynamic via QtSql | None | n/a | `sqlite3[json1]` |
| 16 | zlib | 1.3.2 | zlib license (Gailly/Adler) | Dynamic | Preserve copyright notice | Gailly + Adler | vcpkg default |
| 17 | brotli | 1.2.0 | MIT (Brotli Authors) | Dynamic via freetype/Qt | Preserve copyright + permission notice | Brotli Authors | vcpkg default |
| 18 | zstd | 1.5.7 | BSD + GPLv2 dual | Dynamic via Qt | Either license's terms; BSD path = preserve notice | Facebook/Meta | vcpkg default |
| 19 | double-conversion | 3.4.0 | BSD (V8 authors) | Dynamic via Qt | Preserve copyright + conditions | V8 project authors | vcpkg default |
| 20 | expat | 2.8.3 | MIT (Thai OSS Center / Expat maintainers) | Dynamic via Qt/fontconfig chain | Preserve copyright + permission notice | As listed | vcpkg default |
| 21 | bzip2 | 1.0.8 | BSD-like (see file) | Dynamic via freetype/Qt | Preserve notice | Julian Seward | `bzip2[tool]` builds exe (tool only) |
| 22 | lz4 | 1.10.0 | BSD (Yann Collet) | Dynamic via libpq chain | Preserve copyright + conditions | Yann Collet | vcpkg default |
| 23 | libpq | 18.4 | PostgreSQL license (permissive) | Dynamic via QtSql-psql driver | Preserve copyright + permission notice | PostgreSQL Global Development Group | `libpq[lz4,openssl,zlib]` |
| 24 | D-Bus | 1.16.2 | AFL-2.1 **or** GPL-2.0+ dual | Dynamic via Qt (`qtbase[dbus]`) | AFL-2.1 path: attribution + license copy | Freedesktop contributors | vcpkg default |
| 25 | md4c | 0.5.3 | MIT | Build-time markdown parser (Qt docs/tools chain) | Preserve copyright + permission notice | Martin Mitas | vcpkg default |
| 26 | opengl / egl-registry | 2022-12-04 / 2025-05-27 | Windows SDK license + Khronos registry terms (see share dir notes) | System (Windows OpenGL) | **[LEGAL]** Confirm Khronos header terms cover binary redistribution if shipping GL-dependent binaries beyond Windows | Khronos Group | vcpkg default |
| 27 | MSVC C++ runtime | VS 18 (19.51) | Visual Studio license (redistributable) | Dynamic (`/MD`, `MSVCP140/VCRUNTIME140*.dll`) | Ship via official VC++ Redistributable or app-local copies per VS license | Microsoft | `MultiThreadedDLL` |
| 28 | Build tools (cmake, meson, vcpkg-*) | various | Not distributed | n/a | None | n/a | n/a |

## Explicit reviews

- **Qt 6 licensing.** Qt 6.11.1 under Commercial / GPL-2.0 / GPL-3.0 / LGPL-3.0 at the
  licensee's choice. This project links Qt **dynamically** (verified DLL imports),
  which is the configuration the LGPL path contemplates. **[LEGAL]** No license has
  been formally selected for this project; counsel must confirm the choice and the
  resulting obligations (license copy, notices, relinking, source offer) before any
  distribution of `spectra_gui` with Qt DLLs.
- **FFmpeg licensing/build configuration.** The installed binary is a GPL-3.0 build
  (`--enable-gpl --enable-version3`, static, with x264/x265 and other GPL components).
  The project does **not** link, embed, or distribute it — it invokes a user-provided
  `ffmpeg`/`ffprobe` as a subprocess. **[LEGAL]** Keep it that way: never bundle this
  binary with releases, and document that users supply their own FFmpeg. If a
  different FFmpeg build (e.g. LGPL) is targeted, re-audit that binary's `-buildconf`.
- **FFT backend license.** Own implementation, no third-party code, no obligations.
- **GPU backend license.** OS system libraries + own HLSL; VkFFT integration does not
  exist (stub). No obligations. Re-audit if a third-party GPU FFT library is adopted.
- **Image/video libraries.** PNG codec is own code. JPEG is only decoded by QtGui for
  GUI image I/O (libjpeg-turbo, BSD). Video encode/decode goes through external
  FFmpeg (see above).

## Items requiring professional legal review before distribution

1. Formal Qt license selection (Commercial vs LGPL-3.0) and fulfillment of its terms.
2. Confirmation that no GPL FFmpeg binary (or GPL-built Qt module) ships with releases.
3. Khronos/OpenGL registry header terms for GL-dependent binaries (item 26).
4. Any future additions: GPU FFT library, static Qt linkage, bundled FFmpeg.
