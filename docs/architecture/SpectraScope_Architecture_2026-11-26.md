# SpectraScope — System Architecture

**Project:** Spectral Visualization Generator (SpectraScope)  
**Repository:** [`nishanth-kkj9/SpectralVisualizationGenerator`](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator)  
**Reviewed source snapshot:** `main` at `94809ef237eeb74dfcb05c0dcc269061adc11df3` (`fix(mel): reject unsafe frequency transforms`)  
**Review date:** 26 November 2026  
**Document status:** Implementation-grounded architecture of the inspected source snapshot

> This document describes what the inspected source tree implements. It is not a roadmap, an aspirational target design, or a statement that every representation enum or UI option has a corresponding analysis algorithm. Source files at the pinned commit are authoritative where older plans or documentation disagree.

---

## 1. Executive summary

SpectraScope is an offline, file-to-file audio spectral analysis and visualization application. It reads audio streams from audio files and media containers, computes spectral data, and produces static PNG visualizations or silent visualization videos. It is not a music player, DAW, real-time microphone visualizer, or video-frame analysis system: for an audio/video container it extracts the selected audio stream; it does not analyse the source video's image frames or carry its soundtrack into the generated visualization video.

The architecture is centred on one compiled C++20 static library, `SpectralCore`. The command-line program (`spectragen`) and the optional Qt 6 desktop application (`spectra_gui`) are application shells over the same pipeline. Supporting executables provide media diagnostics (`media_probe`) and performance measurements (`spectral_benchmarks`).

The principal processing path is:

1. Validate an application request and output destination.
2. Probe the input with FFprobe and decode the selected audio stream with FFmpeg.
3. Hash the source and construct a canonical `ProjectConfig`.
4. Convert decoded chunks to mono and feed a bounded overlap buffer.
5. Compute STFT frames using a reusable FFT plan/workspace, or aggregate STFT power through a real Mel filterbank.
6. Store the result and its representation-aware axes/metadata in a `SpectralDataset`.
7. Dispatch the dataset to the appropriate CPU, optional GPU, or video renderer.
8. Write to a unique temporary file beside the destination, then replace the destination only after successful completion.

The main architectural boundaries are deliberately explicit: media decoding does not perform spectral analysis; rendering consumes a dataset rather than recomputing its FFT; the GUI does not own DSP logic; and external FFmpeg processes are launched through a dedicated Windows process wrapper rather than a shell command.

## 2. System context and deployment boundary

```text
                 ┌──────────────────────────────┐
                 │       User / automation      │
                 └──────────────┬───────────────┘
                                │
                  ┌─────────────┴──────────────┐
                  │                            │
          ┌───────▼────────┐           ┌───────▼────────┐
          │   spectragen   │           │  spectra_gui   │
          │ CLI / batch    │           │ Qt 6 desktop   │
          └───────┬────────┘           └───────┬────────┘
                  └─────────────┬──────────────┘
                                │ GenerateConfig
                       ┌────────▼─────────┐
                       │   SpectralCore   │
                       │ shared pipeline  │
                       └────────┬─────────┘
                                │
            ┌───────────────────▼───────────────────┐
            │ Validate → decode → analyse → render  │
            │ dataset, metadata, cancellation       │
            └───────┬───────────────────────┬───────┘
                    │                       │
          ┌─────────▼─────────┐   ┌─────────▼──────────┐
          │ FFprobe / FFmpeg  │   │ PNG / D3D11 /      │
          │ external programs │   │ FFmpeg video output│
          └───────────────────┘   └────────────────────┘
```

### 2.1 Runtime components

| Component | Role | Notes |
|---|---|---|
| `spectragen` | Command-line entry point | Single-file jobs, `batch` mode, Ctrl+C cancellation, and the multiband diagnostic mode. It can request GPU spectrogram rendering. |
| `spectra_gui` | Desktop entry point | Built only when CMake finds Qt 6 Core/Gui/Widgets. Collects settings and delegates work to `Spectral::run_job`. |
| `SpectralCore` | Shared static library | Contains the production pipeline, media/process adapters, DSP, dataset, renderers, encoders, batch executor, and project configuration. |
| `media_probe` | Media diagnostic executable | Opens a media file and reports the selected audio stream, codec, sample rate, channels, duration information, and resolved FFmpeg tool paths. |
| `spectral_benchmarks` | Benchmark executable | Runs named DSP, decode, serialization, render, GPU and video benchmarks and writes JSON measurements. |
| FFmpeg / FFprobe | External runtime dependencies | User-installed executables. They are not C++ libraries linked into `SpectralCore` and are not bundled by the project. |
| Qt 6 | Optional GUI dependency | The core and CLI targets do not require Qt. The GUI target and GUI lifecycle test are conditional on Qt being found. |

### 2.2 Platform boundary

The inspected implementation is Windows-targeted. `SafeProcess` uses Win32 process and pipe APIs, output replacement uses Windows file APIs, the CLI uses Windows console cancellation handling, and the accelerated renderer uses Direct3D 11. The presence of non-MSVC warning flags in CMake does not establish Linux or macOS support. The repository CI workflow also runs on Windows.

## 3. Architectural layers and ownership

### 3.1 Application layer

**CLI (`apps/spectragen`)** parses command-line arguments, validates CLI-specific combinations, maps Ctrl+C/Ctrl+Break into an atomic cancellation flag, and invokes the shared pipeline. Batch mode collects files and delegates to `run_batch`. The multiband option is a diagnostic path that prints a comparison table and then renders the primary dataset.

**GUI (`apps/spectrascope`)** is a thin Qt front end. `MainWindow` turns widget state into a `GenerateConfig`, validates it, creates a `Worker`, and runs that worker on a `QThread`. Signals return progress and completion to the UI thread. The window's close handler requests cancellation and defers closing until the worker thread has finished; the window does not intentionally destroy itself while a job is active. The current GUI does not expose the `GenerateConfig::representation` field, so it leaves analysis at the default STFT. In particular, its “Detail” preset chooses the Mel *display scale* (`freq_scale = mel`); it does not select the real Mel filterbank representation. Real Mel analysis can be selected through the CLI's `--representation mel` option or the shared API.

**Utilities** do not create alternative analysis pipelines. `media_probe` uses the media decoder for diagnostics. `spectral_benchmarks` calls benchmark routines against the core implementation.

### 3.2 Shared core layer

| Area | Main implementation | Responsibility and boundary |
|---|---|---|
| Pipeline and request validation | `src/core/pipeline/pipeline.h`, `pipeline.cpp` | Owns `GenerateConfig`, `validate_config`, `analyze_dataset`, `render_dataset`, and `run_job`. It is the orchestration boundary shared by the CLI and GUI. |
| Process execution | `src/core/process/safe_process.h/.cpp` | Starts and manages external processes, standard pipes, exit status, stderr draining, cancellation-aware reads, and cleanup. |
| Media ingestion | `src/core/media/media_decoder.h/.cpp` | Uses FFprobe/FFmpeg to identify and decode an audio stream into chunks. It does not perform STFT or render images. |
| Audio helpers | `src/core/audio/audio_buffer.h` | Provides an interleaved audio container and helper operations such as channel mixing/resampling. It is not the normal full-file storage path in `run_job`. |
| DSP | `src/core/dsp/fft.h`, `fft_plan.h`, `stft.h`, `mel_filterbank.h` | Implements the Fourier transform, windowing/frame math, reusable FFT execution state, and Mel filterbank. |
| Bounded streaming | `src/core/pipeline/streaming_analyzer.h` | Maintains the samples needed for overlapping STFT frames while dropping samples that are no longer needed. |
| Spectral data | `src/core/spectral/spectral_dataset.h/.cpp`, `representation.h` | Owns spectral frames, axes, source/analysis metadata, validation, transformations, and serialization. |
| Project configuration | `src/core/project/project_config.h/.cpp` | Defines the canonical, validated and persistable configuration representation derived from a user request and decoded source metadata. |
| Image rendering | `src/core/rendering/` | Converts an existing `SpectralDataset` to RGBA pixels and PNG. Includes STFT spectrogram, Mel spectrogram, spectrum plot, PNG output, and D3D11 spectrogram rendering. |
| Video output | `src/core/encoding/video_renderer.h/.cpp`, `video_encoder.h/.cpp` | Samples a time window from a dataset, renders each video frame, and pipes raw RGBA frames to FFmpeg. |
| Batch execution | `src/core/batch/batch.h/.cpp` | Scans files, plans output paths, detects collisions, bounds worker count, retries failed jobs, and records per-file results. |

The top-level CMake file compiles the production `.cpp` implementations into `SpectralCore`. Application and test executables link that library instead of recompiling a second copy of the production engine.

## 4. End-to-end processing pipeline

### 4.1 Request validation

The front end supplies a `GenerateConfig`, which is an application-facing request DTO. It contains paths, string options, display/render settings and defaults convenient to a CLI or UI. `validate_config()` rejects invalid values before decode begins. Validation includes input/output presence and output-format/extension agreement, FFT size, window, hop, finite numeric values, dimensions, representation compatibility, and video-only limits when the effective output is video.

The hop value `0` is a request-level sentinel for `fft_size / 2`; the canonical project configuration stores the resolved hop. The accepted window names are Hann, Hamming, Blackman and Rectangular. The analysis sample rate is taken from the decoded source by the normal pipeline; the request does not expose an arbitrary target-rate conversion setting.

### 4.2 Media probe and decode

`MediaDecoder` uses FFprobe to inspect streams and selects the lowest-index usable audio stream. A video container can therefore be an input source when it contains audio, but the selected payload is audio only. If no usable audio stream exists, the operation fails rather than treating a video stream as audio.

FFmpeg is started with an argument vector through `SafeProcess`; decoded PCM is consumed in chunks. Probe duration and total-frame estimates may be unknown or approximate until clean EOF. The analysis timestamps are derived from decoded sample positions, not from a claim that the probe estimate is exact.

Tool resolution supports explicit path overrides, the `FFMPEG_BINARY` / `FFPROBE_BINARY` environment variables, and PATH resolution. The project does not pin an FFmpeg version in the vcpkg manifest or CMake target graph.

### 4.3 Source identity and canonical configuration

After opening the media stream, the pipeline computes a SHA-256 hash of the input file and creates a `ProjectConfig` from the request plus decoded metadata. This canonical form records resolved analysis parameters, input identity/metadata, dynamic range, frequency settings, representation parameters and renderer settings. Analysis and dataset metadata are derived from this canonical configuration rather than from separate, independently maintained mappings.

The application request (`GenerateConfig`) and the persisted/canonical model (`ProjectConfig`) are different abstractions: the former is convenient for front ends; the latter is intended to be explicit, validated and reproducible.

### 4.4 Channel reduction and bounded streaming

The normal analysis path does not load the entire decoded source into a single in-memory audio vector. For each decoded chunk:

- Mono input is appended directly.
- Multichannel input is reduced sample-by-sample using the arithmetic mean across channels.
- The mono samples are appended to `StreamingMonoBuffer`.
- Every complete `N`-sample analysis frame is consumed at the configured hop `H`.
- Samples that are no longer needed for the next overlapping frame are discarded from the working buffer.

The live raw-audio working set is bounded by the frame/overlap buffer and the current decoder chunk; it does not grow linearly with the duration of a normal input file. The `SpectralDataset`, however, retains computed spectral frames and therefore grows with the number of frames and bins/bands. Bounded input buffering must not be confused with constant total job memory.

`AudioBuffer` remains a useful utility abstraction, but it is not the normal `run_job` audio accumulation strategy. `decode_full_mono()` is an explicit full-buffer exception used by the CLI's multiband diagnostic, which requires access to the complete signal for analyses at several FFT sizes. That diagnostic path performs a separate full decode and then obtains its primary dataset through `analyze_dataset`.

### 4.5 FFT/STFT execution

Production analysis uses `FFTPlan` and `FFTWorkspace`:

- `FFTPlan` is created for the selected FFT size and precomputes the bit-reversal swaps and per-stage twiddle factors.
- The plan is immutable and can be shared safely.
- `FFTWorkspace` owns reusable complex scratch buffers for an analysis context; mutable scratch is not a process-global singleton.
- The normal per-frame loop applies the selected window, executes the forward transform using the plan/workspace, and builds the frame values.

The reference `fft()`, `stft_frame()` and `stft_all()` implementations remain available for compatibility and independent test comparisons. They are not the production per-frame hot loop.

For an `N`-point real-input transform, the stored one-sided frequency bins are `N/2 + 1`. The documented frame policy is:

\[
F = \begin{cases}
0, & L < N \\
\left\lfloor\frac{L-N}{H}\right\rfloor+1, & L \ge N
\end{cases}
\]

where `L` is the decoded mono sample count. Frame `f` starts at sample `fH`, and its timestamp is `fH / sample_rate`. Incomplete trailing frames are dropped; the current analysis path does not silently zero-pad the tail.

The STFT one-sided amplitude convention is:

\[
A_k = \frac{|X_k|}{N\,cg}\times
\begin{cases}
1, & k=0\text{ or }k=N/2 \\
2, & 0<k<N/2
\end{cases}
\]

where `cg` is the coherent gain of the selected window. Stored power is `A_k^2`; phase is `atan2(im, re)`. Production arithmetic is float/complex-float, not double precision.

### 4.6 Representation dispatch

The analysis loop shares decode, mono conversion, buffering, windowing, FFT execution, progress and cancellation. Frame construction and representation metadata then branch:

**STFT:** stores one-sided FFT-bin magnitudes, power, phase, derived frame statistics, and optional reassigned time/frequency coordinates. Reassignment is an STFT capability and is rejected with Mel.

**Mel:** builds one triangular Mel filterbank for the analysis job. The Mel bands aggregate the amplitude-corrected one-sided STFT power:

\[
E_b = \sum_k H_b(k)\,A_k^2,\qquad M_b=\sqrt{E_b}
\]

The resulting dataset stores `M_b` as magnitude and `E_b` as power, preserving `power == magnitude^2`. Mel has explicit band-centre frequencies, no phase values, and no reassignment arrays. Filter normalization supports `none`, `slaney`, and `area`.

Mel is a computed representation, not a display setting. The `freq_scale` option only changes how the renderer maps frequencies to pixels. Thus STFT shown on a Mel display scale remains STFT data; Mel analysis remains Mel data even if displayed with another supported scale.

## 5. Spectral dataset and metadata contracts

`SpectralDataset` is the boundary between analysis and visualization. It owns the computed frames plus time/frequency axes, source metadata, channel information, analysis metadata, normalization metadata and representation description. Renderers consume this stored data and do not rerun the FFT.

### 5.1 Representation-aware axes

- For STFT, the frequency axis is the uniform FFT grid derived from sample rate and FFT size.
- For Mel, the axis stores the explicit Mel band centres. Treating the band index as `k * sample_rate / fft_size` would be incorrect.
- `RepresentationInfo` records the kind, bin/band count, coverage, normalization, phase capability, reassignment capability, explicit centres and representation version.
- Dataset validation checks representation metadata against axis dimensions and frame widths. Phase and reassignment data are only valid when the representation declares those capabilities.

### 5.2 Frequency transformations

Frequency slicing and downsampling are supported for STFT, where each bin is itself a sampled frequency coordinate and the retained centre span can be described exactly.

For real Mel datasets, `filter_band()` and `downsample_frequency()` currently fail closed as no-ops and return the input dataset unchanged. The metadata model stores Mel centres and overall coverage but does not retain per-filter left/right edge provenance in `RepresentationInfo`. After dropping filters, preserving the original coverage would overstate what remains, while replacing coverage with the first/last centre would mislabel centres as edges. The implementation therefore refuses to produce a dataset with inaccurate coverage metadata. Enabling such transforms requires storing exact retained-filter edge provenance.

### 5.3 Persistence and identity

The dataset binary format is version 3 and is STFT-only. Binary serialization refuses non-STFT data rather than writing an incomplete representation. Dataset JSON carries representation metadata and explicit centres, so Mel data can be represented in JSON. The current code does not implement the planned binary v4 representation section. A non-STFT `dataset_identity()` is unavailable until that fuller binary representation contract exists; callers must not treat an empty identity as a valid content fingerprint.

`ProjectConfig` is a separate canonical configuration record. It stores analysis and renderer intent; computed dataset axes/centres belong to the dataset, not to the request DTO. Its declared reproducibility tier distinguishes deterministic spectral/static-image results from video encoding: the project contract targets bit-identical spectral data for the same canonical analysis and input samples, and bit-identical static image output for the same dataset/configuration. It explicitly does not promise byte-identical encoded video, because lossy encoders may produce different bytes between runs.

## 6. Rendering and output subsystem

### 6.1 Static PNG renderers

| Renderer | Accepted data | Current behavior |
|---|---|---|
| `SpectrogramRenderer` | STFT | Maps STFT magnitudes to an RGBA spectrogram, supports linear/log/Mel/Bark/ERB/CQT frequency display mappings, dB normalization, interpolation and Viridis/Heat colour maps. It does not compute a new STFT. |
| `MelSpectrogramRenderer` | Mel | Maps explicit Mel bands to image rows and maps output columns to real frame timestamps using the dataset time axis. It is a dedicated CPU path; it does not pretend Mel bands are uniform FFT bins. |
| `SpectrumRenderer` | STFT | Aggregates frame magnitudes (mean/max/peak/first-frame modes) and draws a static frequency spectrum plot. |
| `PNGEncoder` | RGBA image | Writes the completed pixel buffer to a PNG file. |

Frequency scale names such as Bark, ERB and CQT in renderer configuration are display coordinate mappings. They do not indicate that Bark, ERB or CQT spectral analysis has been implemented.

### 6.2 GPU rendering

The GPU path is a Direct3D 11 compute-shader spectrogram renderer on Windows. It accelerates image rendering; it is not a GPU FFT or a GPU spectral-analysis pipeline. The current GPU renderer accepts STFT datasets only. If the D3D11 device/shaders are unavailable or the GPU render path fails, `SpectrogramRenderer::render_gpu()` falls back to the CPU renderer. Cancellation is checked before and after the GPU dispatch; an in-flight dispatch itself is not preempted.

Mel and spectrum rendering do not route through this D3D11 spectrogram path.

### 6.3 Video rendering

`VideoRenderer` supports STFT and Mel datasets. It generates a sequence of output times at the configured constant frame rate. The number of frames is `ceil(total_duration * fps)`, and output frame `f` is rendered at `f / fps`. For each output time, the renderer selects a time window centred around that time, clamps the window to the available dataset duration, builds a time-shifted subset, and dispatches:

- STFT subset → `SpectrogramRenderer`
- Mel subset → `MelSpectrogramRenderer`

Each rendered frame is an RGBA8 image written sequentially to the FFmpeg encoder. `VideoEncoder::write_frame()` currently writes frames in call order; its `frame_index` parameter is not used to seek or timestamp individual writes. FFmpeg receives a raw RGBA stream and encodes it with the configured codec/CRF and an output pixel format of `yuv420p`. The encoder input pixel format is `rgba` by default.

The generated video is a visualization-only video stream. The source audio is not passed as a second FFmpeg input, so the original soundtrack is not muxed into the output. The current video dispatch always uses `VideoRenderer`'s spectrogram-frame path; selecting `visualization = spectrum` does not turn a video job into an animated spectrum plot. Static PNG output is where the `spectrogram` versus `spectrum` renderer choice is applied.

The encoder uses a raw-video input pipe. FFmpeg documents raw video as headerless: frame size and pixel format must be supplied to interpret the bytes, and frame rate is an input parameter. The project supplies these values on the FFmpeg command line rather than writing a separate raw-video header.

Cancellation is checked between generated frames. On cancellation, the encoder child is terminated; the pipeline's temporary-output guard removes the incomplete output.

### 6.4 Output commit semantics

The pipeline validates the output location before decode. It writes the rendered image/video to a unique temporary path in the destination directory. Only after the render/encode stage succeeds does it replace the destination using the Windows replacement path in `output_files.cpp`. It does not intentionally delete the old destination first and then rename the temporary file. A failed or cancelled job should therefore leave the prior valid destination in place and should not expose a partially written file at the final output path.

Supported public output formats are PNG for static images and the configured video containers accepted by output validation (MP4, WebM and MKV). Video encoding requires a working FFmpeg executable and an FFmpeg build with the requested encoder available. Codec token validation prevents malformed option tokens; it is not a guarantee that every codec name is installed in the user's FFmpeg build.

## 7. Process execution, errors and cancellation

### 7.1 `SafeProcess`

All external FFmpeg/FFprobe launches pass through `SafeProcess`. On Windows it creates the child directly using `CreateProcessW` and an argument vector; it does not invoke `cmd.exe`, PowerShell, `_popen` or `system()` for process execution. It manages optional stdin/stdout/stderr pipes, drains captured stderr on a dedicated thread, writes binary frame/PCM data, reports child exit codes, and owns deterministic pipe/process cleanup.

The FFmpeg video encoder's `extra_args` setting is split on whitespace into literal argv elements. It is not a shell command line and embedded quoting is not a general-purpose way to preserve a value containing spaces.

### 7.2 Typed failures

The core returns an `Error` value carrying a subsystem and a `JobError` classification rather than using a single undifferentiated boolean for the whole pipeline. Current classifications distinguish, among others, bad configuration, missing input, decode/probe failure, missing dependency, no audio stream, analysis/render/encode failure and cancellation. Media open statuses are further mapped into the job-level error model.

### 7.3 Cancellation propagation

A caller-owned atomic flag is passed through the pipeline and observed at stage-specific boundaries:

- Media decoder: while reading decoded chunks; the child process is stopped on cancellation.
- Analysis: before processing each complete spectral frame.
- CPU renderers: during row/frame work; incomplete image buffers are cleared on cancellation.
- Video: between frames; the encoder child is aborted.
- Batch: before a file begins and between retry attempts; the same flag reaches active `run_job` calls.
- GUI: Cancel sets the flag. The worker reports completion only after core cleanup; the window defers closing until its thread-finished handler runs.

Cancellation is a separate outcome (`JobError::Cancelled`), not a successful render and not a generic processing error.

## 8. Concurrency and memory model

| Situation | Concurrency model |
|---|---|
| Single CLI job | Executes the shared pipeline synchronously on the calling thread, with process I/O and renderer internals managed by their respective components. |
| GUI job | One `Worker` object is moved to a `QThread`; progress/finished signals communicate with the GUI thread. |
| Batch job | `run_batch()` creates a bounded `ThreadPool`; the worker count is clamped to at least one and at most hardware concurrency. Each file receives a copied request and an independent `run_job` invocation. |
| FFT execution | Immutable size-specific plan data plus per-analysis reusable mutable workspace; no shared global FFT scratch buffer. |
| FFmpeg stderr | Drained independently from the main pipeline thread to avoid a full stderr pipe blocking the child. |

Batch collection sorts discovered file paths for deterministic order. Output paths mirror the input tree in folder mode and are prechecked for duplicate target names, so two inputs that would map to the same output are marked as conflicts before work begins. Per-file results record output, success/failure, attempt count and elapsed time. Retries are additional attempts after the first run.

## 9. Build, dependencies and test topology

### 9.1 Build definition

The root `CMakeLists.txt` declares C++20 and a minimum CMake project version of 3.20. It creates `SpectralCore` as a static library and links the executables/tests to it. Qt 6 Core/Gui/Widgets is discovered quietly; if found, the GUI and GUI lifecycle test are added.

The tracked `CMakePresets.json` uses preset schema version 6, a Visual Studio 18 2026 x64 generator, and a machine-specific vcpkg toolchain path (`E:/program_files/vcpkg/...`). CMake's official preset-version table says schema version 6 was added in CMake 3.25. Consequently, the project's `cmake_minimum_required(VERSION 3.20)` and the minimum needed to consume the checked-in preset are not the same thing: use CMake 3.25 or newer for this preset file, even though the project CMake minimum is 3.20.

The checked-in CI workflow takes a separate path: it runs on `windows-latest`, uses the Visual Studio 17 2022 generator, installs FFmpeg, configures through `cmake -S . -B build ...` rather than `cmake --preset`, builds Release, performs a repository hygiene check and runs `ctest --test-dir build -C Release --output-on-failure`. Therefore, CI configuration does not exercise the Visual Studio 18 local preset or its hard-coded local vcpkg path.

### 9.2 External dependencies

- **FFmpeg and FFprobe:** required external executables for media ingestion and video encoding; user supplied and resolved at runtime.
- **Qt 6:** optional at configure time; supplied through the vcpkg manifest (`qtbase` with GUI and Widgets features) for the GUI build.
- **D3D11 runtime/driver:** needed for actual GPU rendering; CPU fallback remains available.

### 9.3 Tests

The CMake file registers focused suites across CLI behavior, media decoding, `SafeProcess`, audio buffers, FFT plans, Mel filterbanks/analysis, DSP accuracy, spectral dataset/representation contracts, rendering, project configuration, video encoder/renderer/CLI, output safety, cancellation, streaming, reproducibility, batch behavior, error classification and hardening. The Qt GUI lifecycle test is conditional on Qt being found. Sanitizer- or compiler-specific test targets are also conditional; the number of active CTest tests therefore depends on the configure environment.

The README's statement that there are “21 tests” is not a reliable current test count against this CMake source. Use the configured build tree's `ctest -N` output to identify the tests actually registered on a particular machine.

## 10. Implemented capability matrix

| Capability | Status in inspected source | Boundary / qualification |
|---|---|---|
| Audio file ingestion | Implemented | FFmpeg/FFprobe external process; first usable audio stream. |
| Audio track from a video container | Implemented | Audio stream only; source video frames are not analysed. |
| Streaming mono conversion and STFT | Implemented | Bounded raw-audio working buffer; dataset retained in memory. |
| Real Mel filterbank analysis | Implemented | Triangular filterbank over STFT power; explicit centres; no phase or reassignment. |
| Time-frequency reassignment | Implemented for STFT | Rejected for Mel. |
| Static STFT spectrogram PNG | Implemented | CPU renderer; optional D3D11 path with CPU fallback. |
| Static Mel spectrogram PNG | Implemented | Dedicated CPU renderer. |
| Static spectrum PNG | Implemented for STFT | Aggregates stored frames; not a second analysis pass. |
| STFT/Mel visualization video | Implemented | Constant-frame-rate visual-only output through FFmpeg. |
| GPU FFT / GPU Mel analysis | Not implemented | D3D11 path accelerates STFT spectrogram rendering only. |
| Bark / ERB filterbank analysis | Not implemented | Display scale mappings exist; enum/display names are not analysis algorithms. |
| Constant-Q transform analysis | Not implemented | CQT-style display mapping/configuration exists; it does not create CQT coefficients. |
| Mel frequency slicing/downsampling | Deliberately refused | No-op until exact per-filter support metadata is retained. |
| Binary persistence of Mel datasets | Not implemented in binary v3 | JSON can carry representation metadata and explicit centres; binary v3 is STFT-only. |
| Real-time microphone visualization | Not implemented | Product is offline file-to-file. |
| Source audio muxed into generated video | Not implemented | Video encoder receives only generated RGBA frames. |

## 11. Important invariants and engineering constraints

1. **One shared production core:** CLI and GUI must call the same pipeline; they should not grow separate DSP or output implementations.
2. **Compute then render:** renderers consume `SpectralDataset`; they must not silently recompute the analysis.
3. **Representation truthfulness:** frequency axes, bin counts, phase, reassignment, and persistence metadata must describe the data actually stored.
4. **Display scale is not representation:** a Mel/Bark/ERB/CQT display mapping does not convert STFT data into a corresponding transform.
5. **Bounded decoded-audio memory is not bounded dataset memory:** input buffering is duration-independent at the raw-sample level, but the retained spectral dataset scales with analysis duration and resolution.
6. **No shell-based process execution:** FFmpeg arguments are passed as argv through `SafeProcess`; this reduces shell interpretation risk but does not sandbox the external executable or make arbitrary FFmpeg options intrinsically safe.
7. **Cancellation is a first-class result:** child processes, temporary outputs, workers and GUI lifetime must be cleaned up before a cancelled job is considered finished.
8. **Never publish an incomplete destination:** output is written to a temporary sibling path and committed after successful rendering/encoding.
9. **Preserve representation metadata under transforms:** if exact transformed coverage cannot be represented, refuse the transform rather than inventing axis values.
10. **External tool behavior is environment-dependent:** installed FFmpeg version/build and codec support are runtime dependencies, not guarantees made by CMake.

## 12. Documentation drift identified during review

The older `docs/architecture/ARCHITECTURE.md` is a scaffold-era description: it calls the FFT plan future work and says Qt, GPU and FFmpeg integration are absent. Those statements do not describe the inspected source.

There is also a narrower conflict in `docs/architecture/spectral-representations.md`: one Phase 8 paragraph says the video renderer is STFT-only, while the current `VideoRenderer` dispatches both STFT and Mel datasets to separate renderers. This architecture document follows the current implementation for that point. The same representation document remains useful for its dataset/coverage contract, especially the deliberate refusal of unsafe Mel frequency transforms.

Some older comments in the representation/project-configuration headers still group real Mel processing with future work. The `ProjectFreqScale` enum does describe display remappings, but the separate `RepresentationKind::Mel` path is already implemented. The implementation boundary is: Mel analysis exists; Bark/ERB/CQT filterbank or CQT analysis does not.

The README's “21 tests” count is also stale relative to the CMake registrations and conditional test topology. The actual active count should be obtained from the configured build with `ctest -N`.

## 13. Verification record and review limitations

This document was checked against the GitHub `main` source snapshot `94809ef237eeb74dfcb05c0dcc269061adc11df3` on 26 November 2026. The review cross-checked the CMake target graph, pipeline API/implementation, process and media adapters, FFT/STFT/Mel code, dataset/representation contracts, render and video dispatch, batch code, CLI/GUI workers, presets, vcpkg manifest, CI workflow and existing architecture notes.

The FFmpeg raw-video contract was cross-checked against the live official FFmpeg formats documentation; Qt thread-affinity/worker-object behavior against the Qt 6.12 documentation; and preset-version compatibility against the current CMake preset-version table. These upstream references explain external framework semantics; repository behavior is still determined by the pinned project source.

**This was a source/documentation audit, not a fresh local build.** No local Release build or CTest run was executed during this review. The GitHub status/workflow-run queries available to the review returned no run records for the inspected commit, so this document does not claim that CI passed for `94809ef`. Any previous local test report should be treated as a report for the exact commit and build configuration on which it was run, not automatically transferred to this snapshot.

## 14. Source map (pinned to the reviewed commit)

All internal links below point to the inspected commit so that the document remains reviewable even after `main` moves.

- [Top-level CMake target graph](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/CMakeLists.txt)
- [Shared pipeline API](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/pipeline/pipeline.h) and [pipeline implementation](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/pipeline/pipeline.cpp)
- [Streaming mono buffer](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/pipeline/streaming_analyzer.h)
- [Media decoder](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/media/media_decoder.cpp) and [process wrapper](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/process/safe_process.cpp)
- [FFT plan](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/dsp/fft_plan.h), [STFT contract](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/dsp/stft.h), and [Mel filterbank](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/dsp/mel_filterbank.h)
- [Representation metadata](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/spectral/representation.h) and [dataset model/serialization](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/spectral/spectral_dataset.cpp)
- [Project configuration](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/project/project_config.cpp)
- [STFT spectrogram renderer](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/rendering/spectrogram_renderer.cpp), [Mel renderer](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/rendering/mel_renderer.cpp), and [spectrum renderer](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/rendering/spectrum_renderer.cpp)
- [Video renderer](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/encoding/video_renderer.cpp) and [video encoder](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/encoding/video_encoder.cpp)
- [Output file safety](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/pipeline/output_files.cpp) and [batch executor](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/src/core/batch/batch.cpp)
- [CLI entry point](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/apps/spectragen/main.cpp), [GUI main window](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/apps/spectrascope/main_window.cpp), and [GUI worker](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/apps/spectrascope/worker.cpp)
- [CMake presets](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/CMakePresets.json), [vcpkg manifest](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/vcpkg.json), and [CI workflow](https://github.com/nishanth-kkj9/SpectralVisualizationGenerator/blob/94809ef237eeb74dfcb05c0dcc269061adc11df3/.github/workflows/ci.yml)

## 15. External technical references

These references were checked during this review for the external behaviors described above:

- [FFmpeg Formats Documentation — rawvideo demuxer](https://ffmpeg.org/ffmpeg-formats.html#rawvideo)
- [FFmpeg command-line documentation](https://ffmpeg.org/ffmpeg.html)
- [Qt 6.12 — QThread](https://doc.qt.io/qt-6.12/qthread.html)
- [Qt 6.12 — Threads and QObjects](https://doc.qt.io/qt-6.12/threads-qobject.html)
- [CMake 4.5.1 — CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)

---

**Recommended repository destination:** replace `docs/architecture/ARCHITECTURE.md` with this file after reviewing it in the repository. This generated copy has not been pushed or applied to GitHub.
