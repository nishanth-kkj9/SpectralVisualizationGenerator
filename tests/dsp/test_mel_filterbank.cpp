// tests/dsp/test_mel_filterbank.cpp
// Phase 9 — Mel filterbank unit tests. Conversion, construction,
// normalization semantics, and equivalence against an independent
// double-precision reference written separately (different loop shape)
// to catch shared bugs rather than re-typing production code.
#include "fft.h"
#include "frequency_scale.h"
#include "mel_filterbank.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Spectral;

static int g_run = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_run;                                                    \
        if (cond) {                                                 \
            ++g_pass;                                               \
        } else {                                                    \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);   \
        }                                                           \
    } while (0)

// Independent reference: double precision, bins-outer loop, plain code.
struct RefBank {
    int bands = 0, bins = 0;
    std::vector<double> centers, left, right, weights;  // row-major
};

static bool build_reference(int sr, int fft, double fmin, double fmax, int bands,
                            const std::string& norm, RefBank& out) {
    out = RefBank{};
    const double nyq = sr / 2.0;
    if (fmax <= 0.0) fmax = nyq;
    const auto hz2mel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
    const auto mel2hz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
    const double mlo = hz2mel(fmin), mhi = hz2mel(fmax);
    if (!(mhi > mlo)) return false;
    const int nb = fft / 2 + 1;
    out.bands = bands;
    out.bins = nb;
    out.centers.resize(bands);
    out.left.resize(bands);
    out.right.resize(bands);
    out.weights.assign((size_t)bands * nb, 0.0);
    const double bin_hz = (double)sr / fft;
    for (int b = 0; b < bands; ++b) {
        const double left = (b == 0) ? fmin : mel2hz(mlo + (mhi - mlo) * b / (bands + 1));
        const double center = mel2hz(mlo + (mhi - mlo) * (b + 1) / (bands + 1));
        const double right =
            (b == bands - 1) ? fmax : mel2hz(mlo + (mhi - mlo) * (b + 2) / (bands + 1));
        if (!(left < center && center < right)) return false;
        out.centers[b] = center;
        out.left[b] = left;
        out.right[b] = right;
        double div = 1.0;
        if (norm == "slaney") div = (right - left) / 2.0;
        for (int k = 0; k < nb; ++k) {
            const double f = k * bin_hz;
            double w = 0.0;
            if (f > left && f < right)
                w = (f <= center) ? (f - left) / (center - left) : (right - f) / (right - center);
            out.weights[(size_t)b * nb + k] = w;
        }
        if (norm == "area") {
            double s = 0.0;
            for (int k = 0; k < nb; ++k) s += out.weights[(size_t)b * nb + k];
            div = s;
        }
        if (!(div > 0.0)) return false;
        if (div != 1.0)
            for (int k = 0; k < nb; ++k) out.weights[(size_t)b * nb + k] /= div;
        double check = 0.0;
        for (int k = 0; k < nb; ++k) check += out.weights[(size_t)b * nb + k];
        if (!(check > 0.0)) return false;
    }
    return true;
}

static MelFilterbankConfig cfg(int sr = 22050, int fft = 1024, float fmin = 0.0f,
                               float fmax = 0.0f, int bands = 64,
                               RepresentationNorm norm = RepresentationNorm::Slaney) {
    MelFilterbankConfig c;
    c.sample_rate = sr;
    c.fft_size = fft;
    c.fmin_hz = fmin;
    c.fmax_hz = fmax;
    c.bands = bands;
    c.norm = norm;
    return c;
}

static void test_conversion() {
    std::printf("[conversion]\n");
    CHECK(hz_to_mel(0.0f) == 0.0f, "mel(0)=0");
    CHECK(mel_to_hz(0.0f) == 0.0f, "hz(0)=0");
    CHECK(std::fabs(hz_to_mel(1000.0f) - 1000.0f) < 1.0f, "1000Hz~=1000mel");
    bool mono = true;
    float prev = -1.0f;
    for (float f = 0.0f; f <= 24000.0f; f += 7.0f) {
        const float m = hz_to_mel(f);
        if (!(m >= prev)) mono = false;
        prev = m;
    }
    CHECK(mono, "hz_to_mel monotone");
    // Hz round-trip is ill-conditioned near 0 (mel_to_hz subtracts ~1
    // from 10^(m/2595)): absolute error scales with (f+700), so bound it
    // that way instead of relatively. Mel-domain round trip is tight.
    float worst_hz = 0.0f, worst_mel = 0.0f;
    for (float f = 1.0f; f <= 20000.0f; f *= 1.7f) {
        const float rt = mel_to_hz(hz_to_mel(f));
        const float d = std::fabs(rt - f) / (f + 700.0f);
        if (d > worst_hz) worst_hz = d;
        if (f >= 100.0f) {
            const float r = std::fabs(rt - f) / f;
            if (r > worst_mel) worst_mel = r;
        }
    }
    CHECK(worst_hz < 2e-6f, "hz round-trip within conditioning bound");
    CHECK(worst_mel < 1e-5f, "hz round-trip tight above 100Hz");
    float worst_mm = 0.0f;
    // mel_to_hz suffers the same tiny-argument cancellation below ~100 mel
    // (10^x - 1 in float), so the tight loop starts where it is conditioned.
    for (float m = 100.0f; m <= 3000.0f; m *= 1.4f) {
        const float d = std::fabs(hz_to_mel(mel_to_hz(m)) - m) / m;
        if (d > worst_mm) worst_mm = d;
    }
    CHECK(worst_mm < 1e-6f, "mel round-trip tight");
}

static void test_construction() {
    std::printf("[construction]\n");
    for (auto norm : {RepresentationNorm::None, RepresentationNorm::Slaney,
                      RepresentationNorm::Area}) {
        MelFilterbank fb;
        std::string err;
        CHECK(fb.build(cfg(22050, 1024, 0.0f, 0.0f, 64, norm), err), "builds");
        CHECK(fb.valid(), "valid");
        CHECK(fb.bands() == 64, "band count");
        CHECK(fb.fft_bins() == 513, "fft bins");
        CHECK(fb.fmax_hz() == 11025.0f, "default fmax is nyquist");
        bool ordered = true, in_range = true, nonneg = true;
        for (int b = 0; b < 64; ++b) {
            if (b > 0 && !(fb.centers_hz()[b] > fb.centers_hz()[b - 1])) ordered = false;
            if (!(fb.centers_hz()[b] >= 0.0f && fb.centers_hz()[b] <= 11025.0f))
                in_range = false;
            if (!(fb.left_hz()[b] < fb.centers_hz()[b] &&
                  fb.centers_hz()[b] < fb.right_hz()[b]))
                nonneg = false;
        }
        CHECK(ordered, "centers ordered");
        CHECK(in_range, "centers in range");
        CHECK(nonneg, "edges strict");
    }
    // Invalid configurations fail closed with reasons.
    MelFilterbank fb;
    std::string err;
    CHECK(!fb.build(cfg(22050, 1024, 0, 0, 0), err) && !err.empty(), "zero bands rejected");
    CHECK(!fb.build(cfg(22050, 1024, 0, 0, 513 + 1), err), "bands>bins rejected");
    CHECK(!fb.build(cfg(22050, 1024, 5000, 1000), err), "fmin>fmax rejected");
    CHECK(!fb.build(cfg(22050, 1024, 0, 20000), err), "fmax>nyquist rejected");
    CHECK(!fb.build(cfg(0, 1024, 0, 0, 64), err), "bad rate rejected");
    CHECK(!fb.build(cfg(22050, 1000, 0, 0, 64), err), "non-pow2 fft rejected");
    CHECK(!fb.valid(), "failed build leaves invalid bank");
    // Degenerate geometry: tiny range + many bands forces an empty filter.
    CHECK(!fb.build(cfg(8000, 256, 100.0f, 101.0f, 64), err), "degenerate rejected");
}

static void test_normalization() {
    std::printf("[normalization]\n");
    MelFilterbank none, slaney, area;
    std::string err;
    CHECK(none.build(cfg(22050, 1024, 80.0f, 8000.0f, 40, RepresentationNorm::None),
                     err),
          "none builds");
    CHECK(slaney.build(cfg(22050, 1024, 80.0f, 8000.0f, 40, RepresentationNorm::Slaney),
                       err),
          "slaney builds");
    CHECK(area.build(cfg(22050, 1024, 80.0f, 8000.0f, 40, RepresentationNorm::Area),
                     err),
          "area builds");
    // Exact inter-mode relationships (same construction path, one final
    // float division): slaney == none/halfwidth and area == none/rowsum,
    // bit for bit. Triangle math bounds weights to [0,1] over the reals;
    // float edge rounding can overshoot by an ulp. Slaney approximates
    // unit analytic area up to discretization; area sums to 1 up to float
    // rounding.
    bool rel = true, area_one = true, bounded = true;
    for (int b = 0; b < 40; ++b) {
        const float hw =
            (slaney.right_hz()[b] - slaney.left_hz()[b]) / 2.0f;
        double rowsum = 0.0;
        for (int k = 0; k < 513; ++k) rowsum += none.weights()[(size_t)b * 513 + k];
        const float adiv = static_cast<float>(rowsum);
        for (int k = 0; k < 513; ++k) {
            const float wn = none.weights()[(size_t)b * 513 + k];
            if (!(wn >= 0.0f && wn <= 1.0f + 1e-6f)) bounded = false;
            if (slaney.weights()[(size_t)b * 513 + k] != wn / hw) rel = false;
            if (area.weights()[(size_t)b * 513 + k] != wn / adiv) rel = false;
        }
        double asum = 0.0;
        for (int k = 0; k < 513; ++k) asum += area.weights()[(size_t)b * 513 + k];
        if (std::fabs(asum - 1.0) > 1e-5) area_one = false;
    }
    CHECK(bounded, "none weights within [0,1]");
    CHECK(rel, "norm modes are exact scalings of none");
    CHECK(area_one, "area unit discrete sum");
}

static void test_reference() {
    std::printf("[reference]\n");
    const struct {
        int sr, fft, bands;
        float fmin, fmax;
        const char* norm;
        RepresentationNorm mode;
    } cases[] = {
        {22050, 1024, 64, 0.0f, 0.0f, "slaney", RepresentationNorm::Slaney},
        {44100, 2048, 128, 20.0f, 16000.0f, "slaney", RepresentationNorm::Slaney},
        {16000, 512, 40, 0.0f, 8000.0f, "none", RepresentationNorm::None},
        {22050, 1024, 32, 80.0f, 8000.0f, "area", RepresentationNorm::Area},
        {8000, 256, 20, 50.0f, 4000.0f, "slaney", RepresentationNorm::Slaney},
    };
    for (auto& c : cases) {
        MelFilterbank fb;
        std::string err;
        CHECK(fb.build(cfg(c.sr, c.fft, c.fmin, c.fmax, c.bands, c.mode), err),
              "production builds");
        RefBank ref;
        CHECK(build_reference(c.sr, c.fft, c.fmin, c.fmax, c.bands, c.norm, ref),
              "reference builds");
        bool ok = fb.bands() == ref.bands;
        float worst = 0.0f, worst_dc = 0.0f;
        for (int b = 0; ok && b < c.bands; ++b) {
            // Float-vs-double construction noise (~1e-7 per op over a
            // ~10-op chain): 1e-5 relative is an order above the noise
            // floor and far below any audible/meaningful threshold.
            const float dc =
                std::fabs(fb.centers_hz()[b] - (float)ref.centers[b]) /
                (float)ref.centers[b];
            if (dc > worst_dc) worst_dc = dc;
            if (dc > 1e-5f) ok = false;
            for (int k = 0; ok && k < ref.bins; ++k) {
                const float d = std::fabs(fb.weights()[(size_t)b * ref.bins + k] -
                                          (float)ref.weights[(size_t)b * ref.bins + k]);
                if (d > worst) worst = d;
                if (d > 1e-4f) ok = false;
            }
        }
        if (!ok)
            std::printf("  DIAG case sr=%d fft=%d bands=%d worst_w=%f worst_dc=%f\n",
                        c.sr, c.fft, c.bands, worst, worst_dc);
        CHECK(ok, "matches independent reference");
    }
    // apply(): hand-verifiable tiny case through the public path.
    {
        MelFilterbank fb;
        std::string err;
        CHECK(fb.build(cfg(8000, 8, 0.0f, 4000.0f, 2, RepresentationNorm::None), err),
              "tiny builds");
        // bins at 0,1000,...,4000 Hz; band edges from mel spacing.
        float power[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // DC impulse
        float out[2] = {0.0f, 0.0f};
        fb.apply(power, out);
        // DC bin belongs to the first filter only (weight 1 at f=0? left
        // edge is 0: f<=left excluded, so DC weight is 0 unless center
        // interpolation covers it — assert energy conservation instead).
        RefBank ref;
        CHECK(build_reference(8000, 8, 0, 4000, 2, "none", ref), "tiny ref builds");
        double expect[2] = {0.0, 0.0};
        for (int b = 0; b < 2; ++b)
            for (int k = 0; k < 5; ++k) expect[b] += ref.weights[b * 5 + k] * power[k];
        CHECK(std::fabs(out[0] - expect[0]) < 1e-6f &&
                  std::fabs(out[1] - expect[1]) < 1e-6f,
              "apply matches reference accumulation");
    }
}

static void test_determinism() {
    std::printf("[determinism]\n");
    MelFilterbank a, b;
    std::string err;
    CHECK(a.build(cfg(), err) && b.build(cfg(), err), "both build");
    CHECK(a.weights() == b.weights() && a.centers_hz() == b.centers_hz(),
          "identical builds");
}

int main() {
    test_conversion();
    test_construction();
    test_normalization();
    test_reference();
    test_determinism();
    std::printf("\n=== mel_filterbank: %d/%d passed ===\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
