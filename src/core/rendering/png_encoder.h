#pragma once

// Minimal PNG encoder (no external dependencies).
// Writes 8-bit truecolor (RGB) and 8-bit truecolor+alpha (RGBA) PNGs using
// DEFLATE-stored blocks (no compression). Files are larger but always valid
// and dependency-free.

#include <cstdint>
#include <string>
#include <vector>

namespace Spectral {

class PNGEncoder {
public:
    // Write RGBA8 image (width x height, length = width*height*4).
    // Returns true on success.
    static bool write_rgba(const std::string& path,
                           int width, int height,
                           const uint8_t* pixels);

    // Write RGB8 image (width x height, length = width*height*3).
    static bool write_rgb(const std::string& path,
                          int width, int height,
                          const uint8_t* pixels);

    // In-memory form (for tests / golden fixtures)
    static bool encode_rgba(std::vector<uint8_t>& out,
                            int width, int height,
                            const uint8_t* pixels);

    // Decode a PNG file into RGBA8. Used by golden-image tests.
    // Returns true on success. Allocates `out_pixels` (width*height*4).
    static bool read_rgba(const std::string& path,
                          int& width, int& height,
                          std::vector<uint8_t>& out_pixels);
};

} // namespace Spectral
