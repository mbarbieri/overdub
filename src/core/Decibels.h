#pragma once

#include <cmath>

// Decibels and linear amplitude gain, for Song settings (Track volume, Clip gain), the engine's
// mix law and the UI's meters. Realtime-safe: no allocation, locks or logging.
namespace overdub::core
{
// 10^(db / 20): 0 dB is 1.0, -6.0206 dB is 0.5.
inline double decibelsToGain (double db)
{
    return std::pow (10.0, db / 20.0);
}

// 20 log10 (gain): the inverse of decibelsToGain; -inf for a gain of 0.
inline double gainToDecibels (double gain)
{
    return 20.0 * std::log10 (gain);
}
} // namespace overdub::core
