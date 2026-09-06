# Spectral Visualization Generator - Architecture

## Module Boundaries

### `src/core/media/`
- **MediaDecoder**: Abstract interface for media file decoding. Responsible for reading audio/video frames from file. Does NOT perform DSP or spectral computation.
- **AudioBuffer**: Container for decoded audio samples. Holds channel-interleaved or planar float data with sample rate metadata. No processing logic.

### `src/core/audio/`
- **AudioBuffer** (defined here): Implementation details, resampling (if needed), channel conversion.

### `src/core/dsp/`
- **DSP Pipeline**: High-level orchestration of signal processing. Defines the sequence of operations (windowing, filtering) but defers actual transforms to lower-level components.
- **FFT Plan** (future): Abstracted FFT plan interface. Not implemented in this phase.

### `src/core/spectral/`
- **SpectralAnalyzer**: Computes spectral representation from audio buffer. STFT, windowing, and magnitude computation. Does not store state between calls.
- **SpectralDataset**: Holds and manages a collection of computed spectral frames. Provides indexing, filtering, and access patterns for renderers. Owns the reusable spectral data (the "compute once → store → render many times" principle).

### `src/core/rendering/`
- **SpectralRenderer**: Renders spectral data to image/video output. Uses pre-computed SpectralDataset. Does not recompute DSP.

### `src/core/export/`
- **Exporter**: Writes rendered output to file format (PNG, EXR, MP4, etc.). Consumes rendered image data from SpectralRenderer.

### `src/cli/`
- Command-line entry point. Uses the same core implementation as GUI. minimal, no GUI dependencies.

### `src/gui/`
- GUI entry point (future). Uses core interfaces to display and interact. Not yet integrated.

## Data Flow

```
Media file
    ↓ (MediaDecoder)
Audio frames
    ↓ (Audio processing / AudioBuffer)
Digitized audio samples
    ↓ (SpectralAnalyzer -> STFT)
Spectral frames
    ↓ (SpectralDataset - store)
Reusable spectral data
    ↓ (SpectralRenderer)
Rendered image
    ↓ (Exporter)
Output file (PNG/MP4)
```

## Key Principles

1. **Compute once → store reusable spectral data → render many times**
2. **DSP engine must not depend on GUI**
3. **Renderer must not secretly recompute DSP**
4. **CLI and GUI must use the same core implementation**
5. **No FFT/STFT integration in this phase** - interfaces defined, implementation deferred
6. **No Qt/GPU/FFmpeg integration in this phase** - pure C++ foundation