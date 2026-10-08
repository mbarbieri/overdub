#include "ui/TakeLanes.h"

#include <catch2/catch_test_macros.hpp>

// Take Lanes and the Passes list, pure (issue #30, ADR 0005 UI amendment): each lane's heard
// spans, and a lane click -> Swap / bring back / nothing.
using namespace overdub::song;
using namespace overdub::ui;

namespace
{
Take takeOf (const Id& id, const Id& trackId, const Id& passId, SampleCount origin, SampleCount length)
{
    Take take;
    take.id = id;
    take.trackId = trackId;
    take.passId = passId;
    take.lengthSamples = length;
    take.origin = origin;
    return take;
}

Clip clipOf (const Id& id, ClipSource source, SampleCount timelineStart, SampleCount sourceOffset, SampleCount length)
{
    return Clip { .id = id, .source = std::move (source), .timelineStart = timelineStart, .sourceOffset = sourceOffset, .length = length };
}

// Guitar ("g"): Pass 1 [0, 10000) in Clip "c1", Pass 2 [2000, 5000) in Clip "c2" on top, Pass 3
// [12000, 14000) with no Clip; a Backing Track Clip "cb" [11000, 13000) on top of nothing.
// Vocals ("v"): Pass 2 only, Clip "cv".
Song threePassSong()
{
    Song song = makeEmptySong (48000);
    song.passes = { Pass { "p1", 0, "" }, Pass { "p2", 2000, "" }, Pass { "p3", 12000, "" } };
    song.takes = {
        takeOf ("g1", "g", "p1", 0, 10000),
        takeOf ("g2", "g", "p2", 2000, 3000),
        takeOf ("v2", "v", "p2", 2000, 3000),
        takeOf ("g3", "g", "p3", 12000, 2000),
    };
    song.backingTracks.push_back (BackingTrack { .id = "b", .originalFilename = "Band.wav", .lengthSamples = 48000 });

    Track guitar;
    guitar.id = "g";
    guitar.name = "Guitar";
    guitar.clips = {
        clipOf ("c1", { ClipSourceKind::take, "g1" }, 0, 0, 10000),
        clipOf ("c2", { ClipSourceKind::take, "g2" }, 2000, 0, 3000),
        clipOf ("cb", { ClipSourceKind::backing, "b" }, 11000, 0, 2000),
    };
    Track vocals;
    vocals.id = "v";
    vocals.name = "Vocals";
    vocals.clips = { clipOf ("cv", { ClipSourceKind::take, "v2" }, 2000, 0, 3000) };
    song.tracks = { guitar, vocals };
    return song;
}
} // namespace

TEST_CASE ("each Take Lane shows its Take's recorded span and where it is heard, newest Pass first", "[ui][take_lanes]")
{
    const auto rows = takeLaneRows (threePassSong(), "g");
    REQUIRE (rows.size() == 3);

    CHECK (rows[0].takeId == "g3");
    CHECK (rows[0].title == "Pass 3");
    CHECK (rows[0].recorded == SampleSpan { 12000, 14000 });
    CHECK (rows[0].heard.empty());
    CHECK (rows[0].status == HeardStatus::notHeard);

    CHECK (rows[1].takeId == "g2");
    CHECK (rows[1].heard == std::vector<SampleSpan> { { 2000, 5000 } });
    CHECK (rows[1].status == HeardStatus::heard);

    CHECK (rows[2].takeId == "g1");
    CHECK (rows[2].heard == std::vector<SampleSpan> { { 0, 2000 }, { 5000, 10000 } });
    CHECK (rows[2].status == HeardStatus::partlyHeard);
}

TEST_CASE ("a lane click where another Take's Clip sounds swaps that Clip", "[ui][take_lanes]")
{
    const auto song = threePassSong();
    CHECK (laneClickAt (song, "g1", 3000) == LaneClick { LaneClickKind::swap, "c2" });
    CHECK (laneClickAt (song, "g2", 2500) == LaneClick {}); // already heard there
    CHECK (laneClickAt (song, "g2", 4999) == LaneClick {});

    const auto edit = laneEditAt (song, "g1", 3000, "unused");
    REQUIRE (edit.has_value());
    CHECK (edit->label == "Use Pass 1 on Guitar");
    CHECK (edit->clipId == "c2");
    const auto expected = swapClipTake (song, "c2", "g1");
    REQUIRE (expected.has_value());
    CHECK (edit->song == *expected);
}

TEST_CASE ("a lane click where no Take Clip sounds brings the whole Take back on top", "[ui][take_lanes]")
{
    const auto song = threePassSong();
    // Over the Backing Track Clip, and in the gap after it.
    CHECK (laneClickAt (song, "g3", 12500) == LaneClick { LaneClickKind::bringBack, {} });
    CHECK (laneClickAt (song, "g3", 13500) == LaneClick { LaneClickKind::bringBack, {} });

    const auto edit = laneEditAt (song, "g3", 13500, "new-clip");
    REQUIRE (edit.has_value());
    CHECK (edit->label == "Bring back Pass 3 on Guitar");
    CHECK (edit->clipId == "new-clip");
    const auto& clips = edit->song.tracks[0].clips;
    REQUIRE (clips.size() == 4);
    CHECK (clips.back() == clipOf ("new-clip", { ClipSourceKind::take, "g3" }, 12000, 0, 2000));
}

TEST_CASE ("a lane click outside its Take's recorded span does nothing", "[ui][take_lanes]")
{
    const auto song = threePassSong();
    CHECK (laneClickAt (song, "g2", 1999) == LaneClick {});
    CHECK (laneClickAt (song, "g2", 5000) == LaneClick {});
    CHECK (laneClickAt (song, "nope", 3000) == LaneClick {});
    CHECK (! laneEditAt (song, "g2", 6000, "x").has_value());
}

TEST_CASE ("the Passes list shows each Pass newest first with its Tracks and heard status", "[ui][take_lanes]")
{
    auto song = threePassSong();
    const auto rows = passRows (song);
    REQUIRE (rows.size() == 3);
    CHECK (rows[0].passId == "p3");
    CHECK (rows[0].title == "Pass 3 \xC2\xB7 0.250 s \xC2\xB7 Guitar");
    CHECK (rows[0].status == HeardStatus::notHeard);
    CHECK (rows[0].canBringBack);
    CHECK (rows[1].title == "Pass 2 \xC2\xB7 0.041 s \xC2\xB7 Guitar, Vocals");
    CHECK (rows[1].status == HeardStatus::heard);
    CHECK (rows[2].status == HeardStatus::partlyHeard);

    song.tracks.clear();
    const auto orphaned = passRows (song);
    CHECK (orphaned[0].title == "Pass 3 \xC2\xB7 0.250 s \xC2\xB7 no Track");
    CHECK (! orphaned[0].canBringBack);
}
