#pragma once

#include <span>
#include <vector>

// The torch-compatible STFT and iSTFT around the Separation graph (ADR 0007): torch.stft and
// torch.istft with center=True, reflect padding, a periodic Hann window as long as the FFT, and the
// one-sided spectrum. The Python reference (spikes/stem-separation/onnx/reference.py on the
// spike/stem-separation branch) defines "correct".
namespace overdub::separation
{
struct StftConfig
{
    int fftSize = 0; // also the window length
    int hop = 0;
    bool normalized = false;

    int bins() const { return fftSize / 2 + 1; }

    // Frames torch.stft (center=True) makes of `length` samples.
    int framesFor (int length) const { return 1 + length / hop; }
};

// torch.hann_window (length, periodic=True).
std::vector<float> periodicHann (int length);

// The STFT of `signal` into `real` and `imag`, each config.bins() x config.framesFor (signal.size())
// floats laid out [bin][frame] (the graph's [F, T] plane). Requires signal.size() > fftSize / 2
// (reflect padding) and window.size() == fftSize.
void stft (std::span<const float> signal, const StftConfig& config, std::span<const float> window, std::span<float> real, std::span<float> imag);

// The iSTFT of [bin][frame] planes (config.framesFor (out.size()) frames) into `out`, trimmed to
// out.size() samples like torch.istft (length=out.size()).
void istft (std::span<const float> real, std::span<const float> imag, const StftConfig& config, std::span<const float> window, std::span<float> out);
} // namespace overdub::separation
