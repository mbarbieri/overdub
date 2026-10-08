#include "engine/MixLaw.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace overdub::engine
{
float volumeGain (double volumeDb)
{
    return (float) std::pow (10.0, volumeDb / 20.0);
}

ChannelGains panGains (double pan)
{
    const auto clamped = std::clamp (pan, -1.0, 1.0);
    // Exact at the ends: cos (pi / 2) is not exactly 0 in floating point.
    if (clamped <= -1.0)
        return { 1.0f, 0.0f };
    if (clamped >= 1.0)
        return { 0.0f, 1.0f };
    const auto theta = (clamped + 1.0) * std::numbers::pi / 4.0;
    return { (float) std::cos (theta), (float) std::sin (theta) };
}

bool isAudible (const song::Track& track, bool anyTrackSoloed)
{
    if (track.mute)
        return false;
    return ! anyTrackSoloed || track.solo;
}

MixGains mixGains (const song::Song& song)
{
    const auto anySoloed = std::ranges::any_of (song.tracks, &song::Track::solo);
    MixGains gains;
    gains.master = volumeGain (song.master.volumeDb);
    for (const auto& track : song.tracks)
    {
        if (! isAudible (track, anySoloed))
        {
            gains.tracks.push_back ({ 0.0f, 0.0f });
            continue;
        }
        const auto volume = volumeGain (track.volumeDb);
        const auto pan = panGains (track.pan);
        gains.tracks.push_back ({ volume * pan.left, volume * pan.right });
    }
    return gains;
}
} // namespace overdub::engine
