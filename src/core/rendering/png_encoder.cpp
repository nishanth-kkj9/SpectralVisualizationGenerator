#include "png_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace Spectral {

namespace {

// CRC32 (PNG uses standard CRC-32 / ISO 3309 / ITU-T V.42)
struct CRC32 {
    uint32_t table[256];
    CRC32() {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
    }
    uint32_t compute(const uint8_t* p, size_t n) const {
        uint32_t c = 0xffffffffu;
        for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
        return c ^ 0xffffffffu;
    }
};

void append_u32_be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
    v.push_back(static_cast<uint8_t>((x >> 8)  & 0xff));
    v.push_back(static_cast<uint8_t>(x & 0xff));
}

void append_chunk(std::vector<uint8_t>& out, const char type[4],
                  const uint8_t* data, uint32_t length) {
    append_u32_be(out, length);
    const size_t start = out.size();
    out.push_back(static_cast<uint8_t>(type[0]));
    out.push_back(static_cast<uint8_t>(type[1]));
    out.push_back(static_cast<uint8_t>(type[2]));
    out.push_back(static_cast<uint8_t>(type[3]));
    if (length) out.insert(out.end(), data, data + length);
    const CRC32 crc;
    uint32_t c = crc.compute(&out[start], 4 + length);
    append_u32_be(out, c);
}

// DEFLATE stored block (no compression).
void append_stored_block(std::vector<uint8_t>& out, const uint8_t* data, size_t len) {
    // Emit one or more stored blocks, each <= 0xFFFF bytes. The block
    // header is one byte: BFINAL (low bit) | BTYPE (next two bits = 00).
    size_t pos = 0;
    while (pos < len) {
        const size_t remaining = len - pos;
        const bool is_last = remaining <= 0xFFFF;
        const uint16_t chunk = static_cast<uint16_t>(
            is_last ? remaining : 0xFFFF);
        const uint8_t hdr = static_cast<uint8_t>(0x00 | (is_last ? 0x01 : 0x00));
        out.push_back(hdr);
        out.push_back(static_cast<uint8_t>(chunk & 0xff));
        out.push_back(static_cast<uint8_t>((chunk >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(~chunk & 0xff));        // NLEN
        out.push_back(static_cast<uint8_t>((~chunk >> 8) & 0xff));
        out.insert(out.end(), data + pos, data + pos + chunk);
        pos += chunk;
    }
}

void adler32_init(uint32_t& a, uint32_t& b) { a = 1; b = 0; }
void adler32_update(uint32_t& a, uint32_t& b, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
}
uint32_t adler32_final(uint32_t a, uint32_t b) { return (b << 16) | a; }

} // namespace

bool PNGEncoder::encode_rgba(std::vector<uint8_t>& out,
                             int width, int height,
                             const uint8_t* pixels) {
    if (width <= 0 || height <= 0 || pixels == nullptr) return false;
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    const size_t raw_size = (1 + row_bytes) * static_cast<size_t>(height);
    std::vector<uint8_t> raw(raw_size);
    for (int y = 0; y < height; ++y) {
        raw[y * (1 + row_bytes)] = 0;  // filter: None
        std::memcpy(&raw[y * (1 + row_bytes) + 1],
                    pixels + static_cast<size_t>(y) * row_bytes,
                    row_bytes);
    }

    // DEFLATE stream
    std::vector<uint8_t> deflate;
    append_stored_block(deflate, raw.data(), raw.size());

    // Zlib stream = 2-byte header + deflate + 4-byte adler32
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);  // CMF: deflate, window 32K
    zlib.push_back(0x01);  // FLG: no preset dict, no compression
    zlib.insert(zlib.end(), deflate.begin(), deflate.end());
    uint32_t a, b; adler32_init(a, b);
    adler32_update(a, b, raw.data(), raw.size());
    uint32_t ad = adler32_final(a, b);
    append_u32_be(zlib, ad);

    // PNG = signature + IHDR + IDAT + IEND
    out.clear();
    static const uint8_t kSignature[8] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    out.insert(out.end(), kSignature, kSignature + 8);

    // IHDR: 13 bytes
    uint8_t ihdr[13];
    ihdr[0] = static_cast<uint8_t>((width >> 24) & 0xff);
    ihdr[1] = static_cast<uint8_t>((width >> 16) & 0xff);
    ihdr[2] = static_cast<uint8_t>((width >> 8) & 0xff);
    ihdr[3] = static_cast<uint8_t>(width & 0xff);
    ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xff);
    ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xff);
    ihdr[6] = static_cast<uint8_t>((height >> 8) & 0xff);
    ihdr[7] = static_cast<uint8_t>(height & 0xff);
    ihdr[8]  = 8;  // bit depth
    ihdr[9]  = 6;  // color type: RGBA
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    append_chunk(out, "IHDR", ihdr, 13);
    append_chunk(out, "IDAT", zlib.data(), static_cast<uint32_t>(zlib.size()));
    append_chunk(out, "IEND", nullptr, 0);
    return true;
}

bool PNGEncoder::write_rgba(const std::string& path,
                            int width, int height,
                            const uint8_t* pixels) {
    std::vector<uint8_t> data;
    if (!encode_rgba(data, width, height, pixels)) return false;
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return ofs.good();
}

bool PNGEncoder::write_rgb(const std::string& path,
                           int width, int height,
                           const uint8_t* pixels) {
    if (width <= 0 || height <= 0 || pixels == nullptr) return false;
    // Pack RGB->RGBA in a temp buffer (alpha=255)
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < width * height; ++i) {
        rgba[i*4+0] = pixels[i*3+0];
        rgba[i*4+1] = pixels[i*3+1];
        rgba[i*4+2] = pixels[i*3+2];
        rgba[i*4+3] = 255;
    }
    return write_rgba(path, width, height, rgba.data());
}

bool PNGEncoder::read_rgba(const std::string& path,
                           int& width, int& height,
                           std::vector<uint8_t>& out_pixels) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    if (data.size() < 8) return false;
    static const uint8_t kSig[8] = {0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a};
    if (std::memcmp(data.data(), kSig, 8) != 0) return false;

    width = height = 0;
    out_pixels.clear();
    std::vector<uint8_t> raw_scanlines; // concatenated raw (filter+row) bytes

    size_t pos = 8;
    while (pos + 8 <= data.size()) {
        uint32_t len = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos+1]) << 16)
                     | (uint32_t(data[pos+2]) << 8) | uint32_t(data[pos+3]);
        pos += 4;
        char type[5] = {static_cast<char>(data[pos]),
                        static_cast<char>(data[pos+1]),
                        static_cast<char>(data[pos+2]),
                        static_cast<char>(data[pos+3]),
                        0};
        pos += 4;
        if (pos + len + 4 > data.size()) return false;
        const uint8_t* chunk_data = data.data() + pos;
        if (std::strcmp(type, "IHDR") == 0) {
            if (len < 13) return false;
            width  = (chunk_data[0] << 24) | (chunk_data[1] << 16) | (chunk_data[2] << 8) | chunk_data[3];
            height = (chunk_data[4] << 24) | (chunk_data[5] << 16) | (chunk_data[6] << 8) | chunk_data[7];
        } else if (std::strcmp(type, "IDAT") == 0) {
            // Stored-block DEFLATE inside zlib wrapper.
            // zlib: 2-byte header + deflate + 4-byte adler32.
            // deflate (stored blocks): 5-byte block header per 0xFFFF chunk
            //   (BTYPE=00, BFINAL, then LEN, NLEN, data).
            if (len < 6) return false;
            if (chunk_data[0] != 0x78 || chunk_data[1] != 0x01) return false;
            // Deflate payload bounds: [2, len-4)
            size_t off = 2;
            const size_t deflate_end = static_cast<size_t>(len) - 4;
            while (off < deflate_end) {
                if (off + 5 > deflate_end) {
                    std::fprintf(stderr, "DBG: off+5 > deflate_end: off=%zu de=%zu\n", off, deflate_end);
                    return false;
                }
                const uint8_t hdr = chunk_data[off++];
                const uint16_t L = static_cast<uint16_t>(
                    chunk_data[off] | (chunk_data[off + 1] << 8));
                const uint16_t N = static_cast<uint16_t>(
                    chunk_data[off + 2] | (chunk_data[off + 3] << 8));
                off += 4;
                if (static_cast<uint16_t>(~L) != N) {
                    std::fprintf(stderr, "DBG: NLEN mismatch: L=%u ~L=%u N=%u\n", L, (uint16_t)(~L), N);
                    return false;
                }
                if ((hdr >> 1) & 0x03) {
                    std::fprintf(stderr, "DBG: BTYPE != 0: %u\n", (unsigned)((hdr >> 1) & 3));
                    return false;
                }
                if (off + L > deflate_end) {
                    std::fprintf(stderr, "DBG: off+L > deflate_end: off=%zu L=%u de=%zu\n", off, L, deflate_end);
                    return false;
                }
                raw_scanlines.insert(raw_scanlines.end(),
                                     chunk_data + off, chunk_data + off + L);
                off += L;
                if (hdr & 0x01) break;  // BFINAL: last block
            }
        } else if (std::strcmp(type, "IEND") == 0) {
            break;
        }
        pos += len + 4; // skip data + CRC
    }
    if (width <= 0 || height <= 0) return false;
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    if (raw_scanlines.size() != (1 + row_bytes) * static_cast<size_t>(height)) {
        return false;
    }
    out_pixels.assign(width * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t filter = raw_scanlines[y * (1 + row_bytes)];
        if (filter != 0) return false;  // Only None supported in this reader.
        std::memcpy(out_pixels.data() + static_cast<size_t>(y) * row_bytes,
                    raw_scanlines.data() + y * (1 + row_bytes) + 1,
                    row_bytes);
    }
    return true;
}

} // namespace Spectral
