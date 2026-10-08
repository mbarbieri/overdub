#pragma once

#include "song/ClipEdits.h"
#include "song/Song.h"
#include "song/TakeEdits.h"

#include <optional>
#include <string>
#include <vector>

// Take Lanes and the Passes list (issue #30, ADR 0004 amendment for issue #15), pure and without
// JUCE: what each lane under a Track shows, what a click on a lane does to the Song, and the rows
// of Edit's "Passes" menu. The Timeline and the Passes panel only call these.
namespace overdub::ui
{
// One Take Lane: a Take recorded on the Track, its recorded span drawn as a block, the spans where
// it is heard now filled in.
struct TakeLaneRow
{
    song::Id takeId;
    std::string title;                       // "Pass 3"
    std::optional<song::SampleSpan> recorded; // none: the Take has no audio from its Pass start
    std::vector<song::SampleSpan> heard;
    song::HeardStatus status = song::HeardStatus::notHeard;

    bool operator== (const TakeLaneRow&) const = default;
};

// One row per Take recorded on the Track, newest Pass first.
std::vector<TakeLaneRow> takeLaneRows (const song::Song& song, const song::Id& trackId);

enum class LaneClickKind
{
    none,     // outside the Take's recorded span, or the Take is already what sounds there
    swap,     // the top Clip at that point plays another Take: it plays this one instead
    bringBack // no Take Clip sounds there: the whole Take comes back on top
};

struct LaneClick
{
    LaneClickKind kind = LaneClickKind::none;
    song::Id clipId; // the Clip a swap changes

    bool operator== (const LaneClick&) const = default;
};

// What a click on the lane of `takeId` at Song sample `at` does. Only inside the Take's recorded
// span: when the top Clip there plays another Take, swap it; when it plays this Take, nothing;
// otherwise (a gap, a Backing Track Clip) bring the Take back.
LaneClick laneClickAt (const song::Song& song, const song::Id& takeId, song::SampleCount at);

// The Song a lane click makes, its undo label ("Use Pass 2 on Guitar", "Bring back Pass 2 on
// Guitar"), and the Clip it swapped or added (to select).
struct LaneEdit
{
    song::Song song;
    std::string label;
    song::Id clipId;
};

// None when the click does nothing. A bring-back's new Clip gets `newClipId`.
std::optional<LaneEdit> laneEditAt (const song::Song& song, const song::Id& takeId, song::SampleCount at, const song::Id& newClipId);

// A row of Edit's Passes list, newest Pass first.
struct PassRow
{
    song::Id passId;
    std::string title; // "Pass 2 · 1.250 s · Guitar, Vocals" (or "no Track" when every Track is gone)
    song::HeardStatus status = song::HeardStatus::notHeard;
    bool canBringBack = false; // it has a Take on a Track that still exists

    bool operator== (const PassRow&) const = default;
};

std::vector<PassRow> passRows (const song::Song& song);
} // namespace overdub::ui
