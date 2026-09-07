#pragma once

// Strict CLI numeric parsing (Phase 3): every numeric option value goes
// through here. Rejects what atoi/atof silently accept: empty input,
// whitespace padding, partial parses ("123abc"), trailing garbage,
// NaN/infinity, and overflow. Deterministic on Windows/MSVC (no locale,
// no errno surprises beyond explicit ERANGE checks).
//
// Rules: decimal integers `[+-]?[0-9]+` within [lo, hi]; decimal floats
// `[+-]?(digits[.digits] | .digits)([eE][+-]?digits)?`, finite, then
// caller range checks. Hex ("0x10"), NaN/inf spellings, and padded
// input are rejected — flag values must be unambiguous.

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

namespace CliParse {

struct IntResult {
    bool ok = false;
    long long value = 0;
    std::string error;
};

struct FloatResult {
    bool ok = false;
    double value = 0.0;
    std::string error;
};

struct Resolution {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::string error;
};

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Strict long long: full consumption, overflow-safe manual accumulate.
inline IntResult parse_int(const std::string& s, long long lo, long long hi,
                           const std::string& opt) {
    IntResult r;
    if (s.empty()) {
        r.error = opt + " requires an integer value (got empty string)";
        return r;
    }
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') {
        neg = (s[i] == '-');
        if (s.size() == 1) {
            r.error = opt + " requires an integer value (got '" + s + "')";
            return r;
        }
        ++i;
    }
    if (i >= s.size() || !is_digit(s[i])) {
        r.error = opt + " requires an integer value (got '" + s + "')";
        return r;
    }
    // Unsigned accumulate with overflow check before each multiply-add:
    // acc*10+d is safe iff acc <= (2^64-1-d)/10. No UB on huge runs.
    unsigned long long acc = 0;
    for (; i < s.size(); ++i) {
        if (!is_digit(s[i])) {
            r.error = opt + " has trailing characters (got '" + s + "')";
            return r;
        }
        const unsigned long long d = static_cast<unsigned long long>(s[i] - '0');
        if (acc > ((std::numeric_limits<unsigned long long>::max)() - d) / 10ULL) {
            r.error = opt + " integer overflows 64-bit range (got '" + s + "')";
            return r;
        }
        acc = acc * 10ULL + d;
    }
    // Fit into signed range (allow LLONG_MIN magnitude when negative).
    const unsigned long long max_pos =
        static_cast<unsigned long long>((std::numeric_limits<long long>::max)());
    if ((!neg && acc > max_pos) || (neg && acc > max_pos + 1ULL)) {
        r.error = opt + " integer overflows 64-bit range (got '" + s + "')";
        return r;
    }
    long long v;
    if (neg) {
        v = (acc == max_pos + 1ULL) ? (std::numeric_limits<long long>::min)()
                                    : -static_cast<long long>(acc);
    } else {
        v = static_cast<long long>(acc);
    }
    if (v < lo || v > hi) {
        r.error = opt + " out of range [" + std::to_string(lo) + ", " +
                  std::to_string(hi) + "] (got '" + s + "')";
        return r;
    }
    r.ok = true;
    r.value = v;
    return r;
}

// Strict double: decimal shape pre-scan, then strtod for conversion with
// ERANGE + finiteness enforcement. Rejects NaN/inf spellings (no valid
// flag value is non-finite) and hex floats.
inline FloatResult parse_float(const std::string& s, const std::string& opt) {
    FloatResult r;
    if (s.empty()) {
        r.error = opt + " requires a numeric value (got empty string)";
        return r;
    }
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    bool digits = false;
    while (i < s.size() && is_digit(s[i])) {
        ++i;
        digits = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && is_digit(s[i])) {
            ++i;
            digits = true;
        }
    }
    if (!digits) {
        r.error = opt + " requires a numeric value (got '" + s + "')";
        return r;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        bool edigits = false;
        while (i < s.size() && is_digit(s[i])) {
            ++i;
            edigits = true;
        }
        if (!edigits) {
            r.error = opt + " has a malformed exponent (got '" + s + "')";
            return r;
        }
    }
    if (i != s.size()) {
        r.error = opt + " has trailing characters (got '" + s + "')";
        return r;
    }
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end != s.c_str() + s.size()) {
        r.error = opt + " numeric overflow/underflow (got '" + s + "')";
        return r;
    }
    if (!std::isfinite(v)) {
        r.error = opt + " must be finite (got '" + s + "')";
        return r;
    }
    r.ok = true;
    r.value = v;
    return r;
}

// Strict WxH resolution: exactly one x/X separator, strict positive ints.
inline Resolution parse_resolution(const std::string& s) {
    Resolution r;
    size_t x = s.find('x');
    if (x == std::string::npos) x = s.find('X');
    if (x == std::string::npos || x == 0 || x + 1 >= s.size() ||
        s.find('x', x + 1) != std::string::npos ||
        s.find('X', x + 1) != std::string::npos) {
        r.error = "--resolution must be WxH (e.g. 1024x512) (got '" + s + "')";
        return r;
    }
    IntResult w = parse_int(s.substr(0, x), 1, 32768, "--resolution width");
    if (!w.ok) {
        r.error = w.error;
        return r;
    }
    IntResult h = parse_int(s.substr(x + 1), 1, 32768, "--resolution height");
    if (!h.ok) {
        r.error = h.error;
        return r;
    }
    r.ok = true;
    r.width = static_cast<int>(w.value);
    r.height = static_cast<int>(h.value);
    return r;
}

}  // namespace CliParse
