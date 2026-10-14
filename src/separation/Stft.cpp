#include "separation/Stft.h"

#include <pocketfft_hdronly.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

namespace overdub::separation
{
std::vector<float> periodicHann (int length)
{
    std::vector<float> window ((std::size_t) length);
    for (int n = 0; n < length; ++n)
        window[(std::size_t) n] = (float) (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * n / length));
    return window;
}

void stft (std::span<const float> signal, const StftConfig& config, std::span<const float> window, std::span<float> real, std::span<float> imag)
{
    const int length = (int) signal.size();
    const int pad = config.fftSize / 2;
    std::vector<float> padded ((std::size_t) (length + 2 * pad));
    for (int i = 0; i < length + 2 * pad; ++i)
    {
        int source = i - pad;
        if (source < 0)
            source = -source;
        if (source >= length)
            source = 2 * (length - 1) - source;
        padded[(std::size_t) i] = signal[(std::size_t) source];
    }

    const int frames = config.framesFor (length);
    const int bins = config.bins();
    const float scale = config.normalized ? 1.0f / std::sqrt ((float) config.fftSize) : 1.0f;
    std::vector<float> frame ((std::size_t) config.fftSize);
    std::vector<std::complex<float>> spectrum ((std::size_t) bins);
    const pocketfft::shape_t shape { (std::size_t) config.fftSize };
    const pocketfft::stride_t realStride { sizeof (float) };
    const pocketfft::stride_t complexStride { sizeof (std::complex<float>) };
    for (int t = 0; t < frames; ++t)
    {
        for (int n = 0; n < config.fftSize; ++n)
            frame[(std::size_t) n] = padded[(std::size_t) (t * config.hop + n)] * window[(std::size_t) n];
        pocketfft::r2c<float> (shape, realStride, complexStride, 0, pocketfft::FORWARD, frame.data(), spectrum.data(), scale);
        for (int k = 0; k < bins; ++k)
        {
            real[(std::size_t) (k * frames + t)] = spectrum[(std::size_t) k].real();
            imag[(std::size_t) (k * frames + t)] = spectrum[(std::size_t) k].imag();
        }
    }
}

void istft (std::span<const float> real, std::span<const float> imag, const StftConfig& config, std::span<const float> window, std::span<float> out)
{
    const int length = (int) out.size();
    const int frames = config.framesFor (length);
    const int bins = config.bins();
    const int total = config.fftSize + config.hop * (frames - 1);
    std::vector<float> accumulated ((std::size_t) total, 0.0f);
    std::vector<float> envelope ((std::size_t) total, 0.0f);
    std::vector<std::complex<float>> spectrum ((std::size_t) bins);
    std::vector<float> frame ((std::size_t) config.fftSize);
    const float scale = config.normalized ? 1.0f / std::sqrt ((float) config.fftSize) : 1.0f / (float) config.fftSize;
    const pocketfft::shape_t shape { (std::size_t) config.fftSize };
    const pocketfft::stride_t realStride { sizeof (float) };
    const pocketfft::stride_t complexStride { sizeof (std::complex<float>) };
    for (int t = 0; t < frames; ++t)
    {
        for (int k = 0; k < bins; ++k)
            spectrum[(std::size_t) k] = { real[(std::size_t) (k * frames + t)], imag[(std::size_t) (k * frames + t)] };
        pocketfft::c2r<float> (shape, complexStride, realStride, 0, pocketfft::BACKWARD, spectrum.data(), frame.data(), scale);
        for (int n = 0; n < config.fftSize; ++n)
        {
            const auto at = (std::size_t) (t * config.hop + n);
            accumulated[at] += frame[(std::size_t) n] * window[(std::size_t) n];
            envelope[at] += window[(std::size_t) n] * window[(std::size_t) n];
        }
    }
    const int pad = config.fftSize / 2;
    for (int i = 0; i < length; ++i)
        out[(std::size_t) i] = accumulated[(std::size_t) (pad + i)] / envelope[(std::size_t) (pad + i)];
}
} // namespace overdub::separation
