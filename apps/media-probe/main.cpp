// apps/media-probe — minimal media diagnostic: input path, selected audio
// stream, sample rate, channels, duration, and tool diagnostics.
#include "media_decoder.h"

#include <cstdio>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: media_probe <media-file>\n");
        return 2;
    }
    MediaDecoder dec;
    if (!dec.open(argv[1])) {
        std::fprintf(stderr, "media_probe: %s\n", dec.last_error().c_str());
        return 1;
    }
    std::printf("file:      %s\n", argv[1]);
    std::printf("stream:    %d (%s)\n", dec.stream_index(), dec.codec_name().c_str());
    std::printf("rate:      %d Hz\n", dec.sample_rate());
    std::printf("channels:  %d\n", dec.num_channels());
    std::printf("start:     %.6f s\n", dec.start_time());
    if (dec.duration_known())
        std::printf("duration:  %.3f s (probe)\n", dec.duration());
    else
        std::printf("duration:  unknown\n");
    std::printf("ffmpeg:    %s\n", dec.ffmpeg_path().c_str());
    std::printf("ffprobe:   %s\n", dec.ffprobe_path().c_str());
    return 0;
}
