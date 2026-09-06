# Third-Party Notices

Applies to binary distributions of SpectraScope (`spectra_gui`) and the
`spectragen` CLI where noted. Full license texts ship with their packages
(`vcpkg_installed/<triplet>/share/<package>/copyright`) or at the URLs below.

## Dependency closure for spectra_gui (direct + transitive via Qt DLLs)

`spectra_gui.exe` directly imports Qt6Widgets/Qt6Core/Qt6Gui, d3d11,
D3DCOMPILER_47 and the MSVC CRT (verified via `dumpbin /dependents`); the
libraries below are pulled in transitively by the Qt DLLs. The exact shipped
set is fixed at packaging time (e.g. `windeployqt`).

- **Qt 6 (Core, Gui, Widgets, Network, Sql, Test, Concurrent)** — 6.11.1 —
  The Qt Company. Commercial / GPL-2.0 / GPL-3.0 / LGPL-3.0, at the licensee's
  choice. **No license has been selected for this project yet — see
  docs/licensing.md and obtain legal review before distribution.**
  https://www.qt.io/licensing/
- **OpenSSL** — 3.6.3 — Apache License 2.0. https://www.openssl.org/
- **ICU** — 78.3 — Unicode License v3. https://icu.unicode.org/
- **FreeType** — 2.14.3 — FreeType License / GPL dual. https://freetype.org/
- **HarfBuzz** — 14.3.1 — MIT ("Old MIT"). https://harfbuzz.github.io/
- **libpng** — 1.6.58 — libpng license. http://libpng.org/
- **libjpeg-turbo** — 3.2.0 — BSD-style licenses. https://libjpeg-turbo.org/
- **PCRE2** — 10.47 — BSD. https://www.pcre.org/
- **SQLite** — 3.53.4 — Public domain. https://www.sqlite.org/
- **zlib** — 1.3.2 — zlib license. https://zlib.net/
- **brotli** — 1.2.0 — MIT. https://github.com/google/brotli
- **zstd** — 1.5.7 — BSD / GPLv2 dual. https://facebook.github.io/zstd/
- **double-conversion** — 3.4.0 — BSD (V8 authors). https://github.com/google/double-conversion
- **Expat** — 2.8.3 — MIT. https://libexpat.github.io/
- **bzip2** — 1.0.8 — BSD-like. https://sourceware.org/bzip2/
- **LZ4** — 1.10.0 — BSD. https://lz4.github.io/lz4/
- **libpq (PostgreSQL)** — 18.4 — PostgreSQL license. https://www.postgresql.org/
- **D-Bus** — 1.16.2 — AFL-2.1 or GPL-2.0-or-later. https://www.freedesktop.org/wiki/Software/dbus/
- **md4c** — 0.5.3 — MIT. https://github.com/mity/md4c
- **Microsoft Visual C++ Runtime** — dynamic (`MSVCP140`/`VCRUNTIME140*.dll`) —
  Microsoft Visual Studio license; distribute via the official VC++ Redistributable.
- **Windows system components** (D3D11, D3DCompiler, OpenGL, Win32 API) —
  provided by the OS under the Windows license; not redistributed.

## External tools (not linked, not distributed — user-supplied)

- **FFmpeg / ffprobe** — invoked as subprocesses. The build validated against
  FFmpeg 8.1.2 (gyan full build), which is a **GPL-3.0** binary. Do not bundle
  GPL FFmpeg builds with releases. Users supply their own FFmpeg.
  https://ffmpeg.org/

## Embedded data

- **Viridis colormap table** — data from matplotlib, public domain
  (van der Walt & Smith 2015). Attribution retained in source comments.
