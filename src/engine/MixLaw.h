#pragma once

#include "song/Song.h"

#include <vector>

// The Mixer's law (issue #31) as pure functions: what linear gains a Song's volume, pan, mute and
// solo settings give each Track's channels and the Master. Computed off the audio thread; the
// audio thread only multiplies (engine/MixStrip.h).
//
// - Volume: 10^(volumeDb / 20), for Tracks and the Master alike.
// - Pan: the -3 dB constant-power law, theta = (pan + 1) * pi / 4, left = cos (theta),
//   right = sin (theta): centre is sqrt (1/2) (-3.01 dB) on each side, hard left is exactly
//   {1, 0}, hard right exactly {0, 1}. The same law for mono and stereo sources: a mono source
//   feeds both channels, so it is panned; a stereo source keeps its sides, so it is balanced.
// - Mute silences its Track. Solo-in-place: when any Track is soloed, every Track that is not
//   soloed is silent. A Track that is soloed and muted is silent; its solo still silences the
//   Tracks that are not soloed.
namespace overdub::engine
{
// Exact equality compares stored values (tests of the law's exact ends), never computed ones.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
struct ChannelGains
{
    float left = 1.0f;
    float right = 1.0f;

    bool operator== (const ChannelGains&) const = default;
};

// The constant-power pan gains for pan in [-1, 1] (clamped).
ChannelGains panGains (double pan);

// Whether a Track sounds, given whether any Track of its Song is soloed.
bool isAudible (const song::Track& track, bool anyTrackSoloed);

struct MixGains
{
    std::vector<ChannelGains> tracks; // one per Song Track, in Song order
    float master = 1.0f;

    bool operator== (const MixGains&) const = default;
};
#pragma clang diagnostic pop

// Every Track's channel gains (volume x pan, 0 when it does not sound) and the Master's gain.
MixGains mixGains (const song::Song& song);
} // namespace overdub::engine
