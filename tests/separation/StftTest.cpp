#include "separation/Stft.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

using overdub::separation::istft;
using overdub::separation::periodicHann;
using overdub::separation::stft;
using overdub::separation::StftConfig;

namespace
{
constexpr double sampleRate = 44100.0;

std::vector<float> sine (int length, double frequency, float amplitude)
{
    std::vector<float> signal ((std::size_t) length);
    for (int i = 0; i < length; ++i)
        signal[(std::size_t) i] = amplitude * (float) std::sin (2.0 * std::numbers::pi * frequency * i / sampleRate);
    return signal;
}

struct Spectrum
{
    std::vector<float> real;
    std::vector<float> imag;
};

Spectrum spectrumOf (const std::vector<float>& signal, const StftConfig& config)
{
    const auto size = (std::size_t) config.bins() * (std::size_t) config.framesFor ((int) signal.size());
    Spectrum spectrum { std::vector<float> (size), std::vector<float> (size) };
    stft (signal, config, periodicHann (config.fftSize), spectrum.real, spectrum.imag);
    return spectrum;
}
} // namespace

TEST_CASE ("The periodic Hann window matches torch.hann_window (periodic=True)", "[separation][stft]")
{
    const auto window = periodicHann (2048);
    REQUIRE (window.size() == 2048);
    CHECK (window[0] == 0.0f);
    CHECK (window[1024] == Catch::Approx (1.0f).margin (1e-7));
    CHECK (window[512] == Catch::Approx (0.5f).margin (1e-6));
    // Periodic: symmetric around N/2, so w[n] == w[N - n], and w[N - 1] is not 0.
    for (int n = 1; n < 2048; ++n)
        REQUIRE (window[(std::size_t) n] == Catch::Approx (window[(std::size_t) (2048 - n)]).margin (1e-6));
}

TEST_CASE ("STFT then iSTFT of a sine reconstructs it within float tolerance", "[separation][stft]")
{
    const StftConfig config; // n_fft 2048, hop 512: the Separation graph's
    const int length = 44100;
    const auto signal = sine (length, 440.0, 0.5f);
    const auto spectrum = spectrumOf (signal, config);

    std::vector<float> reconstructed ((std::size_t) length);
    istft (spectrum.real, spectrum.imag, config, periodicHann (config.fftSize), reconstructed);

    float worst = 0.0f;
    for (std::size_t i = 0; i < signal.size(); ++i)
        worst = std::max (worst, std::abs (reconstructed[i] - signal[i]));
    CHECK (worst < 1e-5f);
}

TEST_CASE ("The STFT of a sine peaks in its bin with the Hann window's gain", "[separation][stft]")
{
    const StftConfig config;
    const int bin = 100; // 100 x 44100 / 2048 = 2153.3 Hz, exactly on a bin
    const float amplitude = 0.25f;
    const int length = 44100;
    const auto spectrum = spectrumOf (sine (length, bin * sampleRate / config.fftSize, amplitude), config);

    const int frames = config.framesFor (length);
    REQUIRE (frames == 1 + length / config.hop);
    const int frame = frames / 2; // away from the reflect-padded edges
    const auto magnitude = [&] (int k) {
        const auto at = (std::size_t) (k * frames + frame);
        return std::hypot (spectrum.real[at], spectrum.imag[at]);
    };

    int peak = 0;
    for (int k = 1; k < config.bins(); ++k)
        if (magnitude (k) > magnitude (peak))
            peak = k;
    CHECK (peak == bin);
    // A sine of amplitude A on bin k through a periodic Hann window of N: |X[k]| = A x N / 4.
    CHECK (magnitude (bin) == Catch::Approx (amplitude * config.fftSize / 4.0f).epsilon (1e-4));
    // The Hann window leaks only into the neighbouring bins, each at half the peak.
    CHECK (magnitude (bin + 1) == Catch::Approx (amplitude * config.fftSize / 8.0f).epsilon (1e-3));
    CHECK (magnitude (bin + 2) < 1e-2f);
}
