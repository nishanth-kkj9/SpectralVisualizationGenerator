#pragma once

#include "spectral_renderer.h"
#include <string>
#include <vector>

class Exporter {
public:
    virtual ~Exporter() = default;

    // Export a single rendered frame to a file
    // Returns true on success
    virtual bool export_frame(const std::vector<unsigned char>& pixels,
                              int width,
                              int height,
                              const std::string& filepath) = 0;

    // Export all frames as a sequence
    virtual bool export_sequence(const std::vector<std::vector<unsigned char>>& frames,
                                  int width,
                                  int height,
                                  const std::string& filepath_pattern) = 0;

    // Get supported file extensions
    virtual std::vector<std::string> supported_extensions() const = 0;
};