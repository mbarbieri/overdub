#include "engine/BandFilter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace overdub::engine
{
namespace
{
constexpr std::uint64_t onBit = std::uint64_t { 1 } << 63;
constexpr std::uint64_t hzMask = 0x7fffffff;
const double butterworthQ = 1.0 / std::numbers::sqrt2;

struct Trig
{
    double cosine = 1.0;
    double alpha = 0.0;
};

Trig trigFor (double frequencyHz, double sampleRate) noexcept
{
    const auto clamped = std::clamp (frequencyHz, 1.0, 0.45 * sampleRate);
    const auto w0 = 2.0 * std::numbers::pi * clamped / sampleRate;
    return { std::cos (w0), std::sin (w0) / (2.0 * butterworthQ) };
}

BiquadCoefficients normalised (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

double runBiquad (const BiquadCoefficients& c, double& z1, double& z2, double x) noexcept
{
    const auto y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
}
} // namespace

void BandFilterControl::set (BandFilterSetting setting) noexcept
{
    const auto word = setting.on ? onBit | ((std::uint64_t) (setting.lowHz & hzMask) << 32) | (std::uint64_t) (setting.highHz & hzMask) : 0;
    packed.store (word, std::memory_order_relaxed);
}

BandFilterSetting BandFilterControl::get() const noexcept
{
    const auto word = packed.load (std::memory_order_relaxed);
    if ((word & onBit) == 0)
        return {};
    return { true, (int) ((word >> 32) & hzMask), (int) (word & hzMask) };
}

BiquadCoefficients highPassCoefficients (double frequencyHz, double sampleRate) noexcept
{
    const auto [cosine, alpha] = trigFor (frequencyHz, sampleRate);
    return normalised ((1.0 + cosine) / 2.0, -(1.0 + cosine), (1.0 + cosine) / 2.0, 1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
}

BiquadCoefficients lowPassCoefficients (double frequencyHz, double sampleRate) noexcept
{
    const auto [cosine, alpha] = trigFor (frequencyHz, sampleRate);
    return normalised ((1.0 - cosine) / 2.0, 1.0 - cosine, (1.0 - cosine) / 2.0, 1.0 + alpha, -2.0 * cosine, 1.0 - alpha);
}

void BandFilterProcessor::update (BandFilterSetting setting, double sampleRate) noexcept
{
    if (setting == applied && sampleRate == appliedRate)
        return;
    if (setting.on && ! applied.on)
    {
        highPassState = {};
        lowPassState = {};
    }
    applied = setting;
    appliedRate = sampleRate;
    if (setting.on && sampleRate > 0.0)
    {
        highPass = highPassCoefficients (setting.lowHz, sampleRate);
        lowPass = lowPassCoefficients (setting.highHz, sampleRate);
    }
}

void BandFilterProcessor::process (int channel, float* samples, int numSamples) noexcept
{
    if (! applied.on || appliedRate <= 0.0 || channel < 0 || channel > 1)
        return;
    auto& high = highPassState[(std::size_t) channel];
    auto& low = lowPassState[(std::size_t) channel];
    for (int i = 0; i < numSamples; ++i)
    {
        const auto highPassed = runBiquad (highPass, high.z1, high.z2, samples[i]);
        samples[i] = (float) runBiquad (lowPass, low.z1, low.z2, highPassed);
    }
}
} // namespace overdub::engine
