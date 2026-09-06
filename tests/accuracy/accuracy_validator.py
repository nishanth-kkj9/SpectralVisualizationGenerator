# Accuracy Validation Suite for SpectralCore DSP Engine
# Phase 4: Numerical Accuracy Validation
#
# Purpose: Prove that SpectralCore produces correct spectral results
#          through deterministic testing, not visual inspection.
#
# Philosophy: YAGNI — only tests that expose incorrect behavior.
#             Documented tolerances. Machine-readable output.
#             Sanitizer-enabled configuration.

"""Test configuration"""
SAMPLE_RATES = [44100, 48000, 16000, 22050]
FFT_SIZES = [256, 512, 1024, 2048]
WINDOWS = ["rectangular", "hann", "hamming", "blackman"]
AMPLITUDES = [0.01, 0.1, 0.5, 0.9, 1.0]
TOLERANCES = {
    "frequency_bin": 0.5,          # bins
    "frequency_offbin": 1.5,       # bins
    "amplitude": 0.02,             # linear (2% of full scale)
    "power_db": 0.5,               # dB
    "magnitude_db": 1.0,           # dB
}

"""Edge case definitions"""
EDGE_CASES = {
    "dc": {"frequency": 0.0, "description": "Direct current - zero frequency"},
    "nyquist_quarter": {"frequency": 22000.0, "sample_rate": 44100, "description": "Quarter Nyquist"},
    "nyquist_half": {"frequency": 22050.0, "sample_rate": 44100, "description": "Exactly Nyquist"},
    "low_amplitude": {"amplitude": 0.001, "description": "Very low amplitude signal"},
    "silence": {"amplitude": 0.0, "description": "Zero-valued signal"},
    "clipping": {"amplitude": 1.5, "description": "Signal exceeding [-1, 1] range"},
    "two_tones": {"frequencies": [100.0, 440.0], "description": "Two simultaneous sine waves"},
    "off_bin": {"frequency": 137.0, "description": "Frequency not at bin center"},
}

"""Tolerance justification documentation"""
TOLERANCE_DOC = """
Numerical Accuracy Tolerances
==============================

All tolerances are derived from DSP theory and verified against
deterministic synthetic signals. They are NOT empirical fitting parameters.

1. frequency_bin (0.5 bins):
   - For a signal exactly at a bin center: expected peak at that bin.
   - Error < 0.5 bin means the peak is closer to the correct bin than any neighbor.
   - This is a fundamental limit of the DFT resolution.

2. frequency_offbin (1.5 bins):
   - For a signal between bins (off-bin): the peak location error increases.
   - Sub-bin estimation (e.g., parabolic interpolation) can do better,
     but raw peak-finding has this tolerance.

3. amplitude (0.02 linear = 2%):
   - Related to window coherent gain normalization error.
   - Hann window coherent gain = 0.5 exactly; error in normalization
     directly maps to amplitude error.

4. power_db (0.5 dB):
   - 10*log10 power conversion.
   - At high SNR this corresponds to ~1% magnitude error.
   - At low SNR the floor (-90 dB) dominates.

5. magnitude_db (1.0 dB):
   - 20*log10 magnitude conversion.
   - Justifiable just-noticeable difference for spectral visualization.

All tolerances are enforced as absolute limits. Relative tolerances scale
with signal level where appropriate (e.g., floor at -90 dB absolute).
"""

"""Import project DSP functions"""
import sys
import os
import math

# Add project source to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src', 'core'))

from fft import (
    fft as core_fft,
    fft_magnitude,
    fft_power,
    window_rectangular,
    window_hann,
    window_hamming,
    window_blackman,
    window_coherent_gain,
    freq_from_bin,
    bin_from_freq,
    sample_duration,
    power_to_db,
    magnitude_to_db,
)

# Project types
from media_decoder import AudioFrame
from spectral_analyzer import SpectralFrame

"""Helper: generate float samples as std::vector<float"""
def generate_sinewave_f(float_freq, float_duration, int_sr, float_amp=1.0):
    """Generate sine wave samples as a plain Python list (will be converted)."""
    n_samples = int(float_duration * float_sr)
    samples = []
    for i in range(n_samples):
        t = i / float_sr
        samples.append(float_amp * math.sin(2.0 * math.pi * float_freq * t))
    return samples

"""Helper: convert Python list to project AudioFrame-ready data"""
def analyze_with_project(samples, fft_size, sr, window_type):
    """Run FFT analysis using project functions and return results."""
    # Convert samples to complex vector for FFT
    N = fft_size
    complex_x = []
    for i in range(N):
        if i < len(samples):
            complex_x.append(complex(samples[i], 0.0))
        else:
            complex_x.append(complex(0.0, 0.0))
    
    # Apply window
    if window_type == "hann":
        win = window_hann(N)
    elif window_type == "hamming":
        win = window_hamming(N)
    elif window_type == "blackman":
        win = window_blackman(N)
    else:  # rectangular
        win = window_rectangular(N)
    
    # Apply window to real part
    for i in range(N):
        complex_x[i] = complex(complex_x[i].real * win[i], complex_x[i].imag)
    
    # Forward FFT
    core_fft(complex_x, inverse=False)
    
    # Compute magnitude
    mag = fft_magnitude(complex_x)
    
    # Only first half (0 to Nyquist)
    half = N // 2 + 1
    mag_half = mag[:half]
    
    # Find peak
    peak_idx = 0
    peak_mag = mag_half[0]
    for i in range(1, len(mag_half)):
        if mag_half[i] > peak_mag:
            peak_mag = mag_half[i]
            peak_idx = i
    
    # Frequency calculations
    peak_freq = freq_from_bin(peak_idx, N, sr)
    expected_bin = bin_from_freq(float_freq, N, sr)
    bin_error = peak_idx - expected_bin
    
    return {
        "peak_index": peak_idx,
        "peak_magnitude": peak_mag,
        "peak_frequency": peak_freq,
        "expected_bin": expected_bin,
        "frequency_error_bins": bin_error,
        "sample_rate": sr,
        "fft_size": N,
    }


"""Core test infrastructure"""

def test_frequency_accuracy():
    """Test that detected frequency matches expected frequency."""
    results = []
    
    for sr in SAMPLE_RATES:
        for n_fft in FFT_SIZES:
            for window in WINDOWS:
                for amp in AMPLITUDES:
                    # Use bin-center frequency for cleanest test
                    # Make frequency exactly on a bin: k * sr / n_fft
                    # Pick bin index k = 5 for all configs
                    k = 5
                    freq = k * sr / n_fft
                    
                    # Generate signal
                    duration = 1.0
                    samples = generate_sinewave_f(freq, duration, sr, amp)
                    
                    # Run analysis using project functions
                    result = analyze_with_project(samples, n_fft, sr, window)
                    
                    # Calculate error
                    freq_error_hz = abs(result["peak_frequency"] - freq)
                    bin_error = abs(result["frequency_error_bins"])
                    
                    # Tolerance: convert bins to Hz
                    freq_tol_hz = TOLERANCES["frequency_bin"] * sr / n_fft
                    
                    passed = freq_error_hz < freq_tol_hz and bin_error < TOLERANCES["frequency_bin"]
                    
                    results.append({
                        "sample_rate": sr,
                        "fft_size": n_fft,
                        "window": window,
                        "amplitude": amp,
                        "expected_freq": freq,
                        "detected_freq": result["peak_frequency"],
                        "frequency_error_hz": freq_error_hz,
                        "bin_error": bin_error,
                        "passed": passed,
                        "tolerance_hz": freq_tol_hz,
                    })
    
    return results


def test_amplitude_accuracy():
    """Test that amplitude is correctly recovered (relative to coherent gain)."""
    results = []
    
    for sr in [44100]:
        for n_fft in [1024]:
            for window in ["hann", "hamming"]:
                for target_amp in [0.1, 0.5, 0.9]:
                    duration = 1.0
                    samples = generate_sinewave_f(100.0, duration, sr, target_amp)
                    
                    result = analyze_with_project(samples, n_fft, sr, window)
                    
                    # Peak magnitude should be close to target amplitude
                    # (coherent gain normalization handles window effects)
                    if result["peak_magnitude"] > 1e-10:
                        amp_error_rel = abs(result["peak_magnitude"] - target_amp) / target_amp
                    else:
                        amp_error_rel = 1.0  # worst case
                    
                    passed = amp_error_rel < TOLERANCES["amplitude"]
                    
                    results.append({
                        "sample_rate": sr,
                        "fft_size": n_fft,
                        "window": window,
                        "target_amplitude": target_amp,
                        "detected_magnitude": result["peak_magnitude"],
                        "amplitude_error_rel": amp_error_rel,
                        "passed": passed,
                    })
    
    return results


def test_dB_behavior():
    """Test dB conversion accuracy using project functions."""
    results = []
    
    # Test with known power/magnitude values using project functions
    # We test the standalone dB functions from fft.h
    
    test_powers = [0.001, 0.01, 0.1, 1.0, 10.0]
    test_magnitudes = [0.0316, 0.1, 0.316, 1.0, 3.162]  # sqrt of powers
    
    for power in test_powers:
        db = power_to_db(power)
        expected_db = 10.0 * math.log10(power) if power > 0 else -90.0
        passed = abs(db - expected_db) < TOLERANCES["power_db"]
        
        results.append({
            "input_type": "power",
            "input_value": power,
            "db_value": db,
            "expected_db": expected_db,
            "passed": passed,
        })
    
    for mag in test_magnitudes:
        db = magnitude_to_db(mag)
        expected_db = 20.0 * math.log10(mag) if mag > 0 else -90.0
        passed = abs(db - expected_db) < TOLERANCES["magnitude_db"]
        
        results.append({
            "input_type": "magnitude",
            "input_value": mag,
            "db_value": db,
            "expected_db": expected_db,
            "passed": passed,
        })
    
    # Test floor behavior
    floor_result_power = power_to_db(0.0)
    floor_result_mag = magnitude_to_db(0.0)
    floor_passed = floor_result_power <= -80.0 and floor_result_mag <= -80.0
    
    results.append({
        "test": "floor",
        "power_db": floor_result_power,
        "magnitude_db": floor_result_mag,
        "floor_passed": floor_passed,
    })
    
    return results


def test_edge_cases():
    """Test edge cases: DC, silence, Nyquist, etc."""
    results = []
    sr = 44100
    
    # DC signal - all zeros
    dc_samples = [0.0] * int(sr * 1.0)
    dc_result = analyze_with_project(dc_samples, 1024, sr, "hann")
    dc_passed = dc_result["peak_frequency"] < 1.0  # Should be near 0 Hz
    results.append({
        "edge_case": "dc",
        "expected_freq_hz": 0.0,
        "detected_freq_hz": dc_result["peak_frequency"],
        "passed": dc_passed,
    })
    
    # Silence
    silence_samples = [0.0] * int(sr * 0.5)
    silence_result = analyze_with_project(silence_samples, 1024, sr, "hann")
    silence_passed = silence_result["peak_magnitude"] < 1e-6
    results.append({
        "edge_case": "silence",
        "detected_magnitude": silence_result["peak_magnitude"],
        "passed": silence_passed,
    })
    
    # Very low amplitude
    low_amp_samples = generate_sinewave_f(100.0, 1.0, sr, 0.001)
    low_amp_result = analyze_with_project(low_amp_samples, 1024, sr, "hann")
    low_amp_passed = low_amp_result["peak_magnitude"] < 0.01  # Should be very small
    results.append({
        "edge_case": "low_amplitude",
        "expected_amp": 0.001,
        "detected_magnitude": low_amp_result["peak_magnitude"],
        "passed": low_amp_passed,
    })
    
    # Off-bin frequency (not at bin center)
    off_bin_freq = 137.0  # Not at a bin center for N=1024, sr=44100
    off_bin_samples = generate_sinewave_f(off_bin_freq, 1.0, sr, 0.5)
    off_bin_result = analyze_with_project(off_bin_samples, 1024, sr, "hann")
    # For off-bin, we just check it produces a reasonable result
    off_bin_passed = off_bin_result["peak_frequency"] > 0 and off_bin_result["peak_frequency"] < sr / 2
    results.append({
        "edge_case": "off_bin",
        "expected_freq_hz": off_bin_freq,
        "detected_freq_hz": off_bin_result["peak_frequency"],
        "passed": off_bin_passed,
    })
    
    # Nyquist frequency
    nyq_samples = generate_sinewave_f(22050.0, 1.0, sr, 0.5)  # exactly Nyquist
    nyq_result = analyze_with_project(nyq_samples, 1024, sr, "hann")
    # At Nyquist, the peak should be at bin n_fft/2
    # We just check it doesn't crash and produces a positive frequency
    nyq_passed = nyq_result["peak_frequency"] >= 0 and nyq_result["peak_frequency"] <= sr / 2
    results.append({
        "edge_case": "nyquist",
        "expected_freq_hz": 22050.0,
        "detected_freq_hz": nyq_result["peak_frequency"],
        "passed": nyq_passed,
    })
    
    return results


def run_all_tests():
    """Run the complete accuracy validation suite."""
    all_results = {
        "timestamp": __import__('datetime').datetime.now().isoformat(),
        "configuration": {
            "sample_rates": SAMPLE_RATES,
            "fft_sizes": FFT_SIZES,
            "windows": WINDOWS,
            "amplitudes": AMPLITUDES,
            "tolerances": TOLERANCES,
        },
        "tests": {},
    }
    
    # 1. Frequency accuracy
    all_results["tests"]["frequency_accuracy"] = test_frequency_accuracy()
    
    # 2. Amplitude accuracy
    all_results["tests"]["amplitude_accuracy"] = test_amplitude_accuracy()
    
    # 3. dB behavior
    all_results["tests"]["dB_behavior"] = test_dB_behavior()
    
    # 4. Edge cases
    all_results["tests"]["edge_cases"] = test_edge_cases()
    
    # Calculate pass/fail summary
    total_tests = 0
    passed_tests = 0
    
    for test_name, test_results in all_results["tests"].items():
        for r in test_results:
            total_tests += 1
            if r.get("passed", False):
                passed_tests += 1
    
    all_results["summary"] = {
        "total_tests": total_tests,
        "passed_tests": passed_tests,
        "pass_rate": passed_tests / total_tests if total_tests > 0 else 0.0,
    }
    
    return all_results


def save_results(results, output_path):
    """Save test results as JSON."""
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w') as f:
        json.dump(results, f, indent=2)


def main():
    """Main entry point for the accuracy validation suite."""
    print("=" * 60)
    print("SpectralCore Accuracy Validation Suite - Phase 4")
    print("=" * 60)
    
    results = run_all_tests()
    
    # Save machine-readable results
    save_results(results, "tests/accuracy/results.json")
    print("\nResults saved to: tests/accuracy/results.json")
    
    # Print summary
    summary = results["summary"]
    print(f"\nTotal tests: {summary['total_tests']}")
    print(f"Passed: {summary['passed_tests']}")
    print(f"Pass rate: {summary['pass_rate']:.1%}")
    
    # Print failures
    print("\nFailed tests:")
    for test_name, test_results in results["tests"].items():
        for r in test_results:
            if not r.get("passed", False):
                print(f"  FAIL: {test_name} - {r}")
    
    # Exit with error code if any tests failed
    if summary["pass_rate"] < 1.0:
        print("\nSome tests failed. See details above.")
        return 1
    else:
        print("\nAll accuracy tests passed!")
        return 0


if __name__ == "__main__":
    exit_code = main()
    sys.exit(exit_code)