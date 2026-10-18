#pragma once

#include "song/Peaks.h"
#include "song/Song.h"

#include <cstddef>
#include <functional>

namespace overdub::ui
{
// The peaks of a Clip's source (no channels: draw no waveform). The reference must stay valid
// while the caller paints.
using PeaksLookup = std::function<const song::Peaks&(const song::ClipSource&)>;

// What the Track gestures shared by the Record page and the Edit page (and its Timeline) do.
// The window's Song-bound content (ui/SongView.h) builds one and gives the same instance to both
// pages, which keep a reference to it: every member is set, and it outlives them.
struct TrackActions
{
    std::function<void (const song::Id& trackId, bool armed)> arm;
    std::function<void (const song::Id& trackId, bool mute)> mute;
    std::function<void (const song::Id& trackId, bool solo)> solo;
    // A grip drag dropped the Track at this index of the Track order (a different one).
    std::function<void (const song::Id& trackId, std::size_t toIndex)> moveTrack;
    std::function<void (song::SampleCount sample)> seek;
    // The Song a Mixer gesture in progress would make (OpenSong::previewMix).
    std::function<void (const song::Song& preview)> previewMix;
    PeaksLookup peaksFor;
};
} // namespace overdub::ui
