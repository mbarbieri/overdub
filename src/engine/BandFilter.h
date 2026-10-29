#pragma once

#include <array>
#include <atomic>
#include <cstdint>

// The Focus Track's band filter on the audio path (ADR 0010, issue #234): a second-order
// Butterworth high-pass at the low cut followed by a second-order Butterworth low-pass at the high
// cut (two RBJ-cookbook biquads per channel, Q = 1/sqrt 2, transposed direct form II in double).
// Each cut is -3 dB at its frequency and falls 12 dB per octave beyond it.
//
// Threads: a non-audio thread writes the setting into a BandFilterControl (one atomic word, so the
// audio thread never sees half of a change); the audio thread reads it each block, recomputes the
// coefficients when it changed (keeping the filter state, so a moved cut does not click) and
// clears the state when the filter is turned on.
namespace overdub::engine
{
struct BandFilterSetting
{
    bool on = false;
    int lowHz = 0;
    int highHz = 0;

    bool operator== (const BandFilterSetting&) const = default;
};

class BandFilterControl
{
public:
    void set (BandFilterSetting setting) noexcept;
    BandFilterSetting get() const noexcept;

private:
    std::atomic<std::uint64_t> packed { 0 }; // 0: off
};

struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // normalised: a0 = 1
};

// Butterworth biquads at `frequencyHz`, clamped below Nyquist (0.45 x the rate).
BiquadCoefficients highPassCoefficients (double frequencyHz, double sampleRate) noexcept;
BiquadCoefficients lowPassCoefficients (double frequencyHz, double sampleRate) noexcept;

// Audio thread. Two channels of high-pass then low-pass.
class BandFilterProcessor
{
public:
    // Takes `setting` for the next process call: new coefficients when it changed, cleared state
    // when it turns on.
    void update (BandFilterSetting setting, double sampleRate) noexcept;
    bool isOn() const noexcept { return applied.on; }
    // Filters `numSamples` of `channel` (0 or 1) in place; does nothing while off.
    void process (int channel, float* samples, int numSamples) noexcept;

private:
    struct State
    {
        double z1 = 0.0, z2 = 0.0;
    };

    BandFilterSetting applied;
    double appliedRate = 0.0;
    BiquadCoefficients highPass;
    BiquadCoefficients lowPass;
    std::array<State, 2> highPassState {};
    std::array<State, 2> lowPassState {};
};
} // namespace overdub::engine
