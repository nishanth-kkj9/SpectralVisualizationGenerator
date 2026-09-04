#define NOMINMAX
#include "gpu_spectrogram.h"
#include "frequency_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Spectral {

// ---- Inline HLSL sources (compiled at runtime via D3DCompile) ----
// Kept as string literals to avoid file I/O dependencies.

static const char* kSpectrogramHLSL = R"(
RWStructuredBuffer<float> Output : register(u0);
StructuredBuffer<float> Magnitudes : register(t0);
StructuredBuffer<int> FrameOffsets : register(t1);

cbuffer Params : register(b0)
{
    uint Width;
    uint Height;
    uint NumFrames;
    uint NumBins;
    float DbFloor;
    float DbCeiling;
    float FreqMinHz;
    float FreqMaxHz;
    float FreqResHz;
    int ScaleEnum;
    float CqtCenter;
    float CqtQ;
    int InterpMode;
    float Reference;
};

float hz_to_mel(float hz) { return (hz <= 0.0) ? 0.0 : 2595.0 * log10(1.0 + hz / 700.0); }
float hz_to_bark(float hz) { return (hz <= 0.0) ? 0.0 : 13.0 * atan(0.00076 * hz) + 3.5 * atan(hz * hz / (7500.0 * 7500.0)); }
float hz_to_erb(float hz) { return (hz <= 0.0) ? 0.0 : 24.7 * (4.37 * hz / 1000.0 + 1.0); }
float hz_to_cqt_bin(float hz, float fc, float Q) { return (hz <= 0.0 || fc <= 0.0 || Q <= 0.0) ? 0.0 : Q * log2(hz / fc); }

float hz_to_unit(float hz)
{
    if (FreqMaxHz <= FreqMinHz || hz < 0.0) return 0.0;
    float t = 0.0;
    if (ScaleEnum == 0) { t = (hz - FreqMinHz) / (FreqMaxHz - FreqMinHz); }
    else if (ScaleEnum == 1) { float lo = log10(max(FreqMinHz,1.0)); float hi = log10(max(FreqMaxHz,1.0)); float lf = log10(max(hz,1.0)); t = (hi>lo)?(lf-lo)/(hi-lo):0.0; }
    else if (ScaleEnum == 2) { float mmin=hz_to_mel(FreqMinHz); float mmax=hz_to_mel(FreqMaxHz); t=(mmax>mmin)?(hz_to_mel(hz)-mmin)/(mmax-mmin):0.0; }
    else if (ScaleEnum == 3) { float bmin=hz_to_bark(FreqMinHz); float bmax=hz_to_bark(FreqMaxHz); t=(bmax>bmin)?(hz_to_bark(hz)-bmin)/(bmax-bmin):0.0; }
    else if (ScaleEnum == 4) { float emin=hz_to_erb(FreqMinHz); float emax=hz_to_erb(FreqMaxHz); t=(emax>emin)?(hz_to_erb(hz)-emin)/(emax-emin):0.0; }
    else if (ScaleEnum == 5)
    {
        if (CqtCenter<=0.0||CqtQ<=0.0) { float lo=log10(max(FreqMinHz,1.0)); float hi=log10(max(FreqMaxHz,1.0)); float lf=log10(max(hz,1.0)); t=(hi>lo)?(lf-lo)/(hi-lo):0.0; }
        else { float bmin=hz_to_cqt_bin(FreqMinHz,CqtCenter,CqtQ); float bmax=hz_to_cqt_bin(FreqMaxHz,CqtCenter,CqtQ); float bcur=hz_to_cqt_bin(hz,CqtCenter,CqtQ); t=(bmax>bmin)?(bcur-bmin)/(bmax-bmin):0.0; }
    }
    return clamp(t, 0.0, 1.0);
}

float unit_to_hz(float u)
{
    if (u<=0.0) return FreqMinHz; if (u>=1.0) return FreqMaxHz;
    float lo=FreqMinHz, hi=FreqMaxHz;
    for (int i=0; i<40; ++i) { float mid=(lo+hi)*0.5; if(hz_to_unit(mid)<u) lo=mid; else hi=mid; }
    return (lo+hi)*0.5;
}

float mag_to_db(float mag)
{
    if (!isfinite(mag)||mag<=0.0) return DbFloor;
    float ref=max(Reference,1.0); float v=20.0*log10(mag/ref);
    return (!isfinite(v))?DbFloor:v;
}

float norm_db(float db)
{
    if (!isfinite(db)) return 0.0;
    if (DbCeiling<=DbFloor||db<DbFloor) return 0.0;
    if (db>DbCeiling) return 1.0;
    return (db-DbFloor)/(DbCeiling-DbFloor);
}

float sample_mag(int fi, int bi) { fi=clamp(fi,0,(int)NumFrames-1); bi=clamp(bi,0,(int)NumBins-1); return Magnitudes[FrameOffsets[fi]+bi]; }

[numthreads(16,16,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
    if (tid.x>=Width||tid.y>=Height) return;
    float low_frac=1.0-(float)tid.y/(float)(Height-1);
    float freq=unit_to_hz(low_frac);
    float bin_f=freq/max(FreqResHz,1.0);
    int blo=(int)floor(bin_f); int bhi=blo+1;
    blo=clamp(blo,0,(int)NumBins-1); bhi=clamp(bhi,0,(int)NumBins-1);
    float bt=saturate(bin_f-floor(bin_f));
    float cf=(float)tid.x/max((float)(Width-1),1.0)*(float)(NumFrames-1);
    int fi0=(int)floor(cf); int fi1=min(fi0+1,(int)NumFrames-1);
    float wt=saturate(cf-floor(cf));
    float db;
    if (InterpMode==0) { db=mag_to_db(sample_mag(fi0,blo)); }
    else
    {
        float d0=mag_to_db(sample_mag(fi0,blo)); float d1=mag_to_db(sample_mag(fi0,bhi)); float df0=lerp(d0,d1,bt);
        if (fi1==fi0||wt<=0.0) db=df0;
        else { float d2=mag_to_db(sample_mag(fi1,blo)); float d3=mag_to_db(sample_mag(fi1,bhi)); float df1=lerp(d2,d3,bt); db=lerp(df0,df1,wt); }
    }
    Output[tid.y*Width+tid.x]=norm_db(db);
}
)";

static const char* kColormapHLSL = R"(
StructuredBuffer<float3> ColorLUT : register(t0);
StructuredBuffer<float> Normalized : register(t1);
RWByteAddressBuffer Output : register(u0);

cbuffer Params : register(b0) { uint Width; uint Height; int ColorMapType; };

float3 color_heat(float t)
{
    t=clamp(t,0.0,1.0); float r,g,b;
    if (t<0.33) { float u=t/0.33; r=u*180.0/255.0; g=0; b=0; }
    else if (t<0.66) { float u=(t-0.33)/0.33; r=(180+u*75)/255.0; g=u*60/255.0; b=0; }
    else { float u=(t-0.66)/0.34; r=1; g=(60+u*195)/255.0; b=u*220/255.0; }
    return float3(r,g,b);
}

[numthreads(256,1,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
    uint idx=tid.x; if(idx>=Width*Height) return;
    float t=Normalized[idx]; float3 rgb;
    if (ColorMapType==1) { rgb=color_heat(t); }
    else { float tf=t*255.0; int i0=(int)floor(tf); int i1=min(i0+1,255); float f=tf-(float)i0; rgb=lerp(ColorLUT[i0],ColorLUT[i1],f); }
    uint r=(uint)(saturate(rgb.x)*255.0);
    uint g=(uint)(saturate(rgb.y)*255.0);
    uint b=(uint)(saturate(rgb.z)*255.0);
    uint packed=r|(g<<8)|(b<<16)|(0xFFu<<24);
    Output.Store(idx*4,packed);
}
)";

// ---- Implementation ----

GpuSpectrogram::GpuSpectrogram(D3D11Context& ctx) : ctx_(ctx) {
    if (ctx_.is_available()) ensure_shaders();
}

GpuSpectrogram::~GpuSpectrogram() = default;

bool GpuSpectrogram::is_available() const {
    return ctx_.is_available() && shaders_ready_;
}

bool GpuSpectrogram::ensure_shaders() {
    if (shaders_ready_) return true;

    auto vs_blob = ctx_.compile_shader_source(kSpectrogramHLSL, "main", "cs_5_0");
    if (!vs_blob) { std::fprintf(stderr, "GPU: spectrogram shader compile failed\n"); return false; }
    spectrogram_shader_ = ctx_.create_compute_shader(vs_blob.Get());
    if (!spectrogram_shader_) { std::fprintf(stderr, "GPU: spectrogram shader creation failed\n"); return false; }

    auto cs_blob = ctx_.compile_shader_source(kColormapHLSL, "main", "cs_5_0");
    if (!cs_blob) { std::fprintf(stderr, "GPU: colormap shader compile failed\n"); return false; }
    colormap_shader_ = ctx_.create_compute_shader(cs_blob.Get());
    if (!colormap_shader_) { std::fprintf(stderr, "GPU: colormap shader creation failed\n"); return false; }

    shaders_ready_ = true;
    return true;
}

void GpuSpectrogram::build_viridis_lut(std::vector<float>& lut) {
    // Same 256-entry LUT as CPU renderer, converted to float [0,1]
    static const uint8_t kViridis[256][3] = {
        {0x44,0x01,0x54},{0x44,0x02,0x55},{0x44,0x03,0x57},{0x45,0x05,0x58},
        {0x45,0x06,0x5A},{0x45,0x08,0x5B},{0x46,0x09,0x5C},{0x46,0x0B,0x5E},
        {0x46,0x0C,0x5F},{0x46,0x0E,0x61},{0x47,0x0F,0x62},{0x47,0x11,0x63},
        {0x47,0x12,0x65},{0x47,0x14,0x66},{0x47,0x15,0x67},{0x47,0x16,0x69},
        {0x47,0x18,0x6A},{0x48,0x19,0x6B},{0x48,0x1A,0x6C},{0x48,0x1C,0x6E},
        {0x48,0x1D,0x6F},{0x48,0x1E,0x70},{0x48,0x20,0x71},{0x48,0x21,0x72},
        {0x48,0x22,0x73},{0x48,0x23,0x74},{0x47,0x25,0x75},{0x47,0x26,0x76},
        {0x47,0x27,0x77},{0x47,0x28,0x78},{0x47,0x2A,0x79},{0x47,0x2B,0x7A},
        {0x47,0x2C,0x7B},{0x46,0x2D,0x7C},{0x46,0x2F,0x7C},{0x46,0x30,0x7D},
        {0x46,0x31,0x7E},{0x45,0x32,0x7F},{0x45,0x34,0x7F},{0x45,0x35,0x80},
        {0x45,0x36,0x81},{0x44,0x37,0x81},{0x44,0x39,0x82},{0x43,0x3A,0x83},
        {0x43,0x3B,0x83},{0x43,0x3C,0x84},{0x42,0x3D,0x84},{0x42,0x3E,0x85},
        {0x42,0x40,0x85},{0x41,0x41,0x86},{0x41,0x42,0x86},{0x40,0x43,0x87},
        {0x40,0x44,0x87},{0x3F,0x45,0x87},{0x3F,0x47,0x88},{0x3E,0x48,0x88},
        {0x3E,0x49,0x89},{0x3D,0x4A,0x89},{0x3D,0x4B,0x89},{0x3D,0x4C,0x89},
        {0x3C,0x4D,0x8A},{0x3C,0x4E,0x8A},{0x3B,0x50,0x8A},{0x3B,0x51,0x8A},
        {0x3A,0x52,0x8B},{0x3A,0x53,0x8B},{0x39,0x54,0x8B},{0x39,0x55,0x8B},
        {0x38,0x56,0x8B},{0x38,0x57,0x8C},{0x37,0x58,0x8C},{0x37,0x59,0x8C},
        {0x36,0x5A,0x8C},{0x36,0x5B,0x8C},{0x35,0x5C,0x8C},{0x35,0x5D,0x8C},
        {0x34,0x5E,0x8D},{0x34,0x5F,0x8D},{0x33,0x60,0x8D},{0x33,0x61,0x8D},
        {0x32,0x62,0x8D},{0x32,0x63,0x8D},{0x31,0x64,0x8D},{0x31,0x65,0x8D},
        {0x31,0x66,0x8D},{0x30,0x67,0x8D},{0x30,0x68,0x8D},{0x2F,0x69,0x8D},
        {0x2F,0x6A,0x8D},{0x2E,0x6B,0x8E},{0x2E,0x6C,0x8E},{0x2E,0x6D,0x8E},
        {0x2D,0x6E,0x8E},{0x2D,0x6F,0x8E},{0x2C,0x70,0x8E},{0x2C,0x71,0x8E},
        {0x2C,0x72,0x8E},{0x2B,0x73,0x8E},{0x2B,0x74,0x8E},{0x2A,0x75,0x8E},
        {0x2A,0x76,0x8E},{0x2A,0x77,0x8E},{0x29,0x78,0x8E},{0x29,0x79,0x8E},
        {0x28,0x7A,0x8E},{0x28,0x7A,0x8E},{0x28,0x7B,0x8E},{0x27,0x7C,0x8E},
        {0x27,0x7D,0x8E},{0x27,0x7E,0x8E},{0x26,0x7F,0x8E},{0x26,0x80,0x8E},
        {0x26,0x81,0x8E},{0x25,0x82,0x8E},{0x25,0x83,0x8D},{0x24,0x84,0x8D},
        {0x24,0x85,0x8D},{0x24,0x86,0x8D},{0x23,0x87,0x8D},{0x23,0x88,0x8D},
        {0x23,0x89,0x8D},{0x22,0x89,0x8D},{0x22,0x8A,0x8D},{0x22,0x8B,0x8D},
        {0x21,0x8C,0x8D},{0x21,0x8D,0x8C},{0x21,0x8E,0x8C},{0x20,0x8F,0x8C},
        {0x20,0x90,0x8C},{0x20,0x91,0x8C},{0x1F,0x92,0x8C},{0x1F,0x93,0x8B},
        {0x1F,0x94,0x8B},{0x1F,0x95,0x8B},{0x1F,0x96,0x8B},{0x1E,0x97,0x8A},
        {0x1E,0x98,0x8A},{0x1E,0x99,0x8A},{0x1E,0x99,0x8A},{0x1E,0x9A,0x89},
        {0x1E,0x9B,0x89},{0x1E,0x9C,0x89},{0x1E,0x9D,0x88},{0x1E,0x9E,0x88},
        {0x1E,0x9F,0x88},{0x1E,0xA0,0x87},{0x1F,0xA1,0x87},{0x1F,0xA2,0x86},
        {0x1F,0xA3,0x86},{0x20,0xA4,0x85},{0x20,0xA5,0x85},{0x21,0xA6,0x85},
        {0x21,0xA7,0x84},{0x22,0xA7,0x84},{0x23,0xA8,0x83},{0x23,0xA9,0x82},
        {0x24,0xAA,0x82},{0x25,0xAB,0x81},{0x26,0xAC,0x81},{0x27,0xAD,0x80},
        {0x28,0xAE,0x7F},{0x29,0xAF,0x7F},{0x2A,0xB0,0x7E},{0x2B,0xB1,0x7D},
        {0x2C,0xB1,0x7D},{0x2E,0xB2,0x7C},{0x2F,0xB3,0x7B},{0x30,0xB4,0x7A},
        {0x32,0xB5,0x7A},{0x33,0xB6,0x79},{0x35,0xB7,0x78},{0x36,0xB8,0x77},
        {0x38,0xB9,0x76},{0x39,0xB9,0x76},{0x3B,0xBA,0x75},{0x3D,0xBB,0x74},
        {0x3E,0xBC,0x73},{0x40,0xBD,0x72},{0x42,0xBE,0x71},{0x44,0xBE,0x70},
        {0x45,0xBF,0x6F},{0x47,0xC0,0x6E},{0x49,0xC1,0x6D},{0x4B,0xC2,0x6C},
        {0x4D,0xC2,0x6B},{0x4F,0xC3,0x69},{0x51,0xC4,0x68},{0x53,0xC5,0x67},
        {0x55,0xC6,0x66},{0x57,0xC6,0x65},{0x59,0xC7,0x64},{0x5B,0xC8,0x62},
        {0x5E,0xC9,0x61},{0x60,0xC9,0x60},{0x62,0xCA,0x5F},{0x64,0xCB,0x5D},
        {0x67,0xCC,0x5C},{0x69,0xCC,0x5B},{0x6B,0xCD,0x59},{0x6D,0xCE,0x58},
        {0x70,0xCE,0x56},{0x72,0xCF,0x55},{0x74,0xD0,0x54},{0x77,0xD0,0x52},
        {0x79,0xD1,0x51},{0x7C,0xD2,0x4F},{0x7E,0xD2,0x4E},{0x81,0xD3,0x4C},
        {0x83,0xD3,0x4B},{0x86,0xD4,0x49},{0x88,0xD5,0x47},{0x8B,0xD5,0x46},
        {0x8D,0xD6,0x44},{0x90,0xD6,0x43},{0x92,0xD7,0x41},{0x95,0xD7,0x3F},
        {0x97,0xD8,0x3E},{0x9A,0xD8,0x3C},{0x9D,0xD9,0x3A},{0x9F,0xD9,0x38},
        {0xA2,0xDA,0x37},{0xA5,0xDA,0x35},{0xA7,0xDB,0x33},{0xAA,0xDB,0x32},
        {0xAD,0xDC,0x30},{0xAF,0xDC,0x2E},{0xB2,0xDD,0x2C},{0xB5,0xDD,0x2B},
        {0xB7,0xDD,0x29},{0xBA,0xDE,0x27},{0xBD,0xDE,0x26},{0xBF,0xDF,0x24},
        {0xC2,0xDF,0x22},{0xC5,0xDF,0x21},{0xC7,0xE0,0x1F},{0xCA,0xE0,0x1E},
        {0xCD,0xE0,0x1D},{0xCF,0xE1,0x1C},{0xD2,0xE1,0x1B},{0xD4,0xE1,0x1A},
        {0xD7,0xE2,0x19},{0xDA,0xE2,0x18},{0xDC,0xE2,0x18},{0xDF,0xE3,0x18},
        {0xE1,0xE3,0x18},{0xE4,0xE3,0x18},{0xE7,0xE4,0x19},{0xE9,0xE4,0x19},
        {0xEC,0xE4,0x1A},{0xEE,0xE5,0x1B},{0xF1,0xE5,0x1C},{0xF3,0xE5,0x1E},
        {0xF6,0xE6,0x1F},{0xF8,0xE6,0x21},{0xFA,0xE6,0x22},{0xFD,0xE7,0x24}
    };
    lut.resize(256 * 3);
    for (int i = 0; i < 256; ++i) {
        lut[i * 3 + 0] = kViridis[i][0] / 255.0f;
        lut[i * 3 + 1] = kViridis[i][1] / 255.0f;
        lut[i * 3 + 2] = kViridis[i][2] / 255.0f;
    }
}

RenderError GpuSpectrogram::render(const SpectralDataset& dataset,
                                   const SpectrogramConfig& cfg, RGBAImage& out) {
    out.clear();
    if (!is_available()) return RenderError::EmptyDataset;
    D3D11Context& ctx = ctx_;
    ID3D11ComputeShader* vs_shader = spectrogram_shader_.Get();
    ID3D11ComputeShader* cs_shader = colormap_shader_.Get();
    if (!vs_shader || !cs_shader) return RenderError::EmptyDataset;
    if (cfg.width <= 0 || cfg.height <= 0) return RenderError::InvalidDimensions;
    if (dataset.frame_count() <= 0 || dataset.num_frequency_bins() <= 0)
        return RenderError::EmptyDataset;

    const int W = cfg.width;
    const int H = cfg.height;
    const int Nf = dataset.frame_count();
    const int Nk = dataset.num_frequency_bins();
    const float nyquist = (cfg.freq_max_hz > 0.0f) ? cfg.freq_max_hz : dataset.nyquist_frequency();
    const float fmin = std::max(1.0f, cfg.freq_min_hz);
    const float fmax = nyquist;
    if (fmax <= fmin) return RenderError::InvalidFrequencyRange;
    const float reference = dataset.normalization_info().reference_amplitude;

    // ---- 1. Flatten magnitudes + frame offsets ----
    std::vector<float> flat_mags;
    std::vector<int32_t> frame_offsets(Nf);
    size_t offset = 0;
    for (int fi = 0; fi < Nf; ++fi) {
        frame_offsets[fi] = static_cast<int32_t>(offset);
        const auto& fr = dataset.frame(fi);
        size_t count = fr.magnitudes.size();
        flat_mags.insert(flat_mags.end(), fr.magnitudes.begin(), fr.magnitudes.end());
        // Pad last frame if needed
        if (fi == Nf - 1 && static_cast<int>(count) < Nk) {
            flat_mags.insert(flat_mags.end(), Nk - count, 0.0f);
        }
        offset += count;
    }
    // Ensure total is Nf * Nk
    size_t expected = static_cast<size_t>(Nf) * Nk;
    if (flat_mags.size() < expected) {
        flat_mags.resize(expected, 0.0f);
    }

    auto mag_buf = ctx.create_buffer(flat_mags.data(),
                                     static_cast<uint32_t>(flat_mags.size() * sizeof(float)));
    auto offset_buf = ctx.create_buffer(frame_offsets.data(),
                                         static_cast<uint32_t>(frame_offsets.size() * sizeof(int32_t)));
    auto normalized_buf = ctx.create_buffer(nullptr,
                                             static_cast<uint32_t>(W * H * sizeof(float)),
                                             D3D11_USAGE_DEFAULT,
                                             D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);

    if (!mag_buf || !offset_buf || !normalized_buf) return RenderError::EmptyDataset;

    // ---- 2. Dispatch spectrogram_cs ----
    // ponytail: 64B to match HLSL cbuffer row packing + D3D11 16B rule
    struct VsParams {
        uint32_t Width, Height, NumFrames, NumBins;
        float DbFloor, DbCeiling, FreqMinHz, FreqMaxHz, FreqResHz;
        int32_t ScaleEnum;
        float CqtCenter, CqtQ;
        int32_t InterpMode;
        float Reference;
        uint32_t Pad[2] = {};
    };
    static_assert(sizeof(VsParams) == 64, "VsParams must be 64B");
    VsParams vs_params = {
        static_cast<uint32_t>(W), static_cast<uint32_t>(H),
        static_cast<uint32_t>(Nf), static_cast<uint32_t>(Nk),
        cfg.db_floor, cfg.db_ceiling, fmin, fmax,
        dataset.frequency_resolution(),
        static_cast<int32_t>(cfg.freq_scale),
        cfg.cqt_center_hz, cfg.cqt_q,
        static_cast<int32_t>(cfg.interpolation),
        reference
    };

    auto vs_cb = ctx.create_buffer(&vs_params, sizeof(VsParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER);

    // Create SRVs for magnitude and offset buffers
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srv_desc.Buffer.FirstElement = 0;
    srv_desc.Buffer.NumElements = static_cast<UINT>(flat_mags.size());

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mag_srv;
    HRESULT hr = ctx.device()->CreateShaderResourceView(mag_buf.Get(), &srv_desc, mag_srv.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    D3D11_SHADER_RESOURCE_VIEW_DESC offset_srv_desc{};
    offset_srv_desc.Format = DXGI_FORMAT_R32_SINT;
    offset_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    offset_srv_desc.Buffer.FirstElement = 0;
    offset_srv_desc.Buffer.NumElements = static_cast<UINT>(frame_offsets.size());

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> offset_srv;
    hr = ctx.device()->CreateShaderResourceView(offset_buf.Get(), &offset_srv_desc, offset_srv.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    // UAV for normalized output
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = static_cast<UINT>(W * H);

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> norm_uav;
    hr = ctx.device()->CreateUnorderedAccessView(normalized_buf.Get(), &uav_desc, norm_uav.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    auto* dc = ctx.context();
    dc->CSSetShader(vs_shader, nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, vs_cb.GetAddressOf());
    ID3D11ShaderResourceView* vs_srvs[] = { mag_srv.Get(), offset_srv.Get() };
    dc->CSSetShaderResources(0, 2, vs_srvs);
    ID3D11UnorderedAccessView* vs_uavs[] = { norm_uav.Get() };
    dc->CSSetUnorderedAccessViews(0, 1, vs_uavs, nullptr);

    UINT gx = (W + 15) / 16;
    UINT gy = (H + 15) / 16;
    dc->Dispatch(gx, gy, 1);

    // ---- 3. Dispatch colormap_cs ----
    // ponytail: shader branches on ColorMapType; Heat computed in-shader.
    // LUT only needed for Viridis.
    std::vector<float> lut;
    build_viridis_lut(lut);

    auto lut_buf = ctx.create_buffer(lut.data(),
                                     static_cast<uint32_t>(lut.size() * sizeof(float)));

    // Output RGBA buffer
    auto rgba_buf = ctx.create_buffer(nullptr,
                                       static_cast<uint32_t>(W * H * 4),
                                       D3D11_USAGE_DEFAULT,
                                       D3D11_BIND_UNORDERED_ACCESS);
    auto staging = ctx.create_staging_buffer(static_cast<uint32_t>(W * H * 4));

    if (!lut_buf || !rgba_buf || !staging) return RenderError::EmptyDataset;

    D3D11_SHADER_RESOURCE_VIEW_DESC lut_srv_desc{};
    lut_srv_desc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
    lut_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    lut_srv_desc.Buffer.FirstElement = 0;
    lut_srv_desc.Buffer.NumElements = 256;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lut_srv;
    hr = ctx.device()->CreateShaderResourceView(lut_buf.Get(), &lut_srv_desc, lut_srv.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    // Reuse normalized buffer as SRV (need to create SRV for it)
    D3D11_SHADER_RESOURCE_VIEW_DESC norm_srv_desc{};
    norm_srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    norm_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    norm_srv_desc.Buffer.FirstElement = 0;
    norm_srv_desc.Buffer.NumElements = static_cast<UINT>(W * H);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> norm_srv;
    hr = ctx.device()->CreateShaderResourceView(normalized_buf.Get(), &norm_srv_desc, norm_srv.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    D3D11_UNORDERED_ACCESS_VIEW_DESC rgba_uav_desc{};
    rgba_uav_desc.Format = DXGI_FORMAT_R32_UINT;
    rgba_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    rgba_uav_desc.Buffer.FirstElement = 0;
    rgba_uav_desc.Buffer.NumElements = static_cast<UINT>(W * H);

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> rgba_uav;
    hr = ctx.device()->CreateUnorderedAccessView(rgba_buf.Get(), &rgba_uav_desc, rgba_uav.GetAddressOf());
    if (FAILED(hr)) return RenderError::EmptyDataset;

    struct CsParams {
        uint32_t Width, Height;
        int32_t ColorMapType;
        int32_t Pad = 0;
    };
    static_assert(sizeof(CsParams) == 16, "CsParams must be 16B");
    CsParams cs_params = {
        static_cast<uint32_t>(W), static_cast<uint32_t>(H),
        static_cast<int32_t>(cfg.color_map)
    };
    auto cs_cb = ctx.create_buffer(&cs_params, sizeof(CsParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER);

    // Unbind previous UAV before binding new ones
    ID3D11UnorderedAccessView* null_uav[] = { nullptr };
    dc->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);

    dc->CSSetShader(cs_shader, nullptr, 0);
    dc->CSSetConstantBuffers(0, 1, cs_cb.GetAddressOf());
    ID3D11ShaderResourceView* cs_srvs[] = { lut_srv.Get(), norm_srv.Get() };
    dc->CSSetShaderResources(0, 2, cs_srvs);
    ID3D11UnorderedAccessView* cs_uavs[] = { rgba_uav.Get() };
    dc->CSSetUnorderedAccessViews(0, 1, cs_uavs, nullptr);

    UINT gx2 = (static_cast<UINT>(W) * H + 255) / 256;
    dc->Dispatch(gx2, 1, 1);

    // ---- 4. Readback ----
    dc->CopyResource(staging.Get(), rgba_buf.Get());

    // Unbind
    dc->CSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    dc->CSSetShaderResources(0, 2, null_srvs);
    dc->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    ID3D11Buffer* null_cb[] = { nullptr };
    dc->CSSetConstantBuffers(0, 1, null_cb);

    out.width = W;
    out.height = H;
    out.pixels.resize(static_cast<size_t>(W) * H * 4);

    if (!ctx.readback_buffer(staging.Get(), out.pixels.data(), static_cast<uint32_t>(W * H * 4))) {
        out.clear();
        return RenderError::EmptyDataset;
    }

    return RenderError::Ok;
}

} // namespace Spectral
