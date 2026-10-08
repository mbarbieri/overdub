#include "song/TakeEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

// Alternate Takes as pure Song operations (issue #30): heard spans and status, Swap, bring back.
using namespace overdub::song;

namespace
{
Take takeOf (const Id& id, const Id& trackId, const Id& passId, SampleCount origin, SampleCount length)
{
    Take take;
    take.id = id;
    take.trackId = trackId;
    take.passId = passId;
    take.file = "audio/" + id + ".wav";
    take.lengthSamples = length;
    take.origin = origin;
    return take;
}

Clip clipOf (const Id& id, const Id& takeId, SampleCount timelineStart, SampleCount sourceOffset, SampleCount length)
{
    return Clip { .id = id, .source = { ClipSourceKind::take, takeId }, .timelineStart = timelineStart, .sourceOffset = sourceOffset, .length = length };
}

// Guitar ("g") recorded three Passes:
// - Pass 1 from 0: take "g1", origin -100, so its recorded span is [0, 10000); Clip "c1" plays it.
// - Pass 2 from 2000: take "g2", recorded span [2000, 5000); Clip "c2" plays it, on top of "c1".
// - Pass 3 from 6000: take "g3", recorded span [6000, 8000); its Clip was deleted.
// Vocals ("v") recorded Pass 2 too: take "v2" [2000, 5000), Clip "cv".
Song threePassSong()
{
    Song song = makeEmptySong (48000);
    song.passes = { Pass { "p1", 0, "" }, Pass { "p2", 2000, "" }, Pass { "p3", 6000, "" } };
    song.takes = {
        takeOf ("g1", "g", "p1", -100, 10100),
        takeOf ("g2", "g", "p2", 1900, 3100),
        takeOf ("v2", "v", "p2", 1900, 3100),
        takeOf ("g3", "g", "p3", 6000, 2000),
    };

    Track guitar;
    guitar.id = "g";
    guitar.name = "Guitar";
    guitar.clips = { clipOf ("c1", "g1", 0, 100, 10000), clipOf ("c2", "g2", 2000, 100, 3000) };
    Track vocals;
    vocals.id = "v";
    vocals.name = "Vocals";
    vocals.clips = { clipOf ("cv", "v2", 2000, 100, 3000) };
    song.tracks = { guitar, vocals };
    return song;
}

const Track& trackWithId (const Song& song, const Id& id)
{
    const auto* track = findTrack (song, id);
    REQUIRE (track != nullptr);
    return *track;
}
} // namespace

TEST_CASE ("a Take's recorded span runs from its Pass start, or its origin if later, to its end", "[song][take_lanes]")
{
    const auto song = threePassSong();
    CHECK (recordedSpan (song, "g1") == SampleSpan { 0, 10000 });
    CHECK (recordedSpan (song, "g2") == SampleSpan { 2000, 5000 });
    CHECK (recordedSpan (song, "g3") == SampleSpan { 6000, 8000 });
    CHECK (! recordedSpan (song, "nope").has_value());

    const auto clip = clipAsRecorded (song.takes[0], 0, "new");
    REQUIRE (clip.has_value());
    CHECK (*clip == clipOf ("new", "g1", 0, 100, 10000));
    CHECK (! clipAsRecorded (song.takes[0], 20000, "new").has_value());
}

TEST_CASE ("the top Clip at a sample is the last Clip covering it", "[song][take_lanes]")
{
    const auto song = threePassSong();
    const auto& guitar = trackWithId (song, "g");
    CHECK (topClipAt (guitar, 0)->id == "c1");
    CHECK (topClipAt (guitar, 1999)->id == "c1");
    CHECK (topClipAt (guitar, 2000)->id == "c2");
    CHECK (topClipAt (guitar, 4999)->id == "c2");
    CHECK (topClipAt (guitar, 5000)->id == "c1");
    CHECK (topClipAt (guitar, 10000) == nullptr);
}

TEST_CASE ("each Take is heard where the top Clip plays it", "[song][take_lanes]")
{
    const auto song = threePassSong();
    CHECK (heardSpans (song, "g1") == std::vector<SampleSpan> { { 0, 2000 }, { 5000, 10000 } });
    CHECK (heardSpans (song, "g2") == std::vector<SampleSpan> { { 2000, 5000 } });
    CHECK (heardSpans (song, "v2") == std::vector<SampleSpan> { { 2000, 5000 } });
    CHECK (heardSpans (song, "g3").empty());

    // Two adjacent Clips of the same Take merge into one span.
    auto split = splitClip (song, "c2", 3000, "c2b");
    REQUIRE (split.has_value());
    CHECK (heardSpans (*split, "g2") == std::vector<SampleSpan> { { 2000, 5000 } });
}

TEST_CASE ("a Pass is heard when fully covered, partly heard when partly, not heard when untouched", "[song][take_lanes]")
{
    const auto song = threePassSong();
    CHECK (passHeardStatus (song, "p2") == HeardStatus::heard);
    CHECK (passHeardStatus (song, "p1") == HeardStatus::partlyHeard);
    CHECK (passHeardStatus (song, "p3") == HeardStatus::notHeard);
    CHECK (passHeardStatus (song, "nope") == HeardStatus::notHeard);

    // Pass 2 heard on Guitar only: partly heard.
    auto vocalsGone = song;
    vocalsGone.tracks[1].clips.clear();
    CHECK (passHeardStatus (vocalsGone, "p2") == HeardStatus::partlyHeard);

    // A Take whose Track is gone does not count: Pass 2 is heard on what remains.
    auto vocalsDeleted = vocalsGone;
    vocalsDeleted.tracks.pop_back();
    CHECK (passHeardStatus (vocalsDeleted, "p2") == HeardStatus::heard);

    // Per Take: Guitar's Pass 1 Take is partly covered by Pass 2's Clip.
    CHECK (takeHeardStatus (song, "g1") == HeardStatus::partlyHeard);
    CHECK (takeHeardStatus (song, "g2") == HeardStatus::heard);
    CHECK (takeHeardStatus (song, "g3") == HeardStatus::notHeard);

    CHECK (toString (HeardStatus::heard) == "heard");
    CHECK (toString (HeardStatus::partlyHeard) == "partly heard");
    CHECK (toString (HeardStatus::notHeard) == "not heard");
}

TEST_CASE ("a Track's Take Lanes list its own Takes, newest Pass first", "[song][take_lanes]")
{
    auto song = threePassSong();
    song.takes.push_back (takeOf ("orphan", "g", "no-such-pass", 0, 100));
    const auto lanes = laneTakes (song, "g");
    std::vector<Id> ids;
    for (const auto* take : lanes)
        ids.push_back (take->id);
    CHECK (ids == std::vector<Id> { "g3", "g2", "g1", "orphan" });
    CHECK (laneTakes (song, "v").size() == 1);
    CHECK (laneTakes (song, "nope").empty());
    CHECK (passNumber (song, "p3") == 3);
    CHECK (passNumber (song, "nope") == 0);
}

TEST_CASE ("Swap changes the Clip's source in place, keeping layer, gain and fades, aligned to the Take", "[song][take_lanes]")
{
    auto song = threePassSong();
    song.tracks[0].clips[1].gainDb = -4.5;
    song.tracks[0].clips[1].fadeInLength = 300;
    song.tracks[0].clips[1].fadeOutLength = 200;

    const auto swapped = swapClipTake (song, "c2", "g1");
    REQUIRE (swapped.has_value());

    auto expected = song;
    auto& clip = expected.tracks[0].clips[1];
    clip.source.id = "g1";
    clip.sourceOffset = 2000 - (-100); // timeline start - the Take's origin
    CHECK (*swapped == expected);
    CHECK (clip.id == "c2"); // still the top layer
}

TEST_CASE ("Swap trims the Clip to the Take's recorded span and clamps its fades", "[song][take_lanes]")
{
    auto song = threePassSong();
    song.tracks[0].clips[0].gainDb = 3.0;
    song.tracks[0].clips[0].fadeInLength = 2500;
    song.tracks[0].clips[0].fadeOutLength = 4000;

    const auto swapped = swapClipTake (song, "c1", "g2");
    REQUIRE (swapped.has_value());
    const auto& clip = swapped->tracks[0].clips[0];
    CHECK (clip.id == "c1"); // still the bottom layer
    CHECK (clip.source.id == "g2");
    CHECK (clip.timelineStart == 2000);
    CHECK (clip.length == 3000);
    CHECK (clip.sourceOffset == 100);
    CHECK (std::abs (clip.gainDb - 3.0) < 1e-12);
    CHECK (clip.fadeInLength == 2500);
    CHECK (clip.fadeOutLength == 500);
}

TEST_CASE ("Swap refuses a Take with no audio under the Clip, and unknown ids", "[song][take_lanes]")
{
    const auto song = threePassSong();
    const auto apart = swapClipTake (song, "c2", "g3");
    REQUIRE (! apart.has_value());
    CHECK (apart.error().code == "song.take_not_under_clip");
    CHECK (apart.error().fields.at ("clip_id") == "c2");
    CHECK (apart.error().fields.at ("take_id") == "g3");

    CHECK (swapClipTake (song, "nope", "g1").error().code == "song.clip_not_found");
    CHECK (swapClipTake (song, "c2", "nope").error().code == "song.take_not_found");
}

TEST_CASE ("bringing back a Take adds it as recorded on the top layer of its Track", "[song][take_lanes]")
{
    const auto song = threePassSong();
    const auto brought = bringBackTake (song, "g1", "back");
    REQUIRE (brought.has_value());

    auto expected = song;
    expected.tracks[0].clips.push_back (clipOf ("back", "g1", 0, 100, 10000));
    CHECK (*brought == expected);
    CHECK (heardSpans (*brought, "g1") == std::vector<SampleSpan> { { 0, 10000 } });

    auto trackGone = song;
    trackGone.tracks.erase (trackGone.tracks.begin());
    const auto missing = bringBackTake (trackGone, "g1", "back");
    REQUIRE (! missing.has_value());
    CHECK (missing.error().code == "song.take_track_missing");
    CHECK (bringBackTake (song, "nope", "back").error().code == "song.take_not_found");
}

TEST_CASE ("bringing back a Pass adds exactly one top-layer Clip per surviving Take, as recorded", "[song][take_lanes]")
{
    auto song = threePassSong();
    song.takes.push_back (takeOf ("x2", "deleted-track", "p2", 1900, 3100));
    song.tracks[0].clips.pop_back(); // Guitar's Pass 2 Clip is gone

    const auto brought = bringBackPass (song, "p2");
    REQUIRE (brought.has_value());

    const auto& guitar = brought->tracks[0].clips;
    const auto& vocals = brought->tracks[1].clips;
    REQUIRE (guitar.size() == 2);
    REQUIRE (vocals.size() == 2);
    CHECK (guitar[0] == song.tracks[0].clips[0]);
    CHECK (vocals[0] == song.tracks[1].clips[0]);
    CHECK (guitar[1].source.id == "g2");
    CHECK (vocals[1].source.id == "v2");
    for (const auto* clip : { &guitar[1], &vocals[1] })
    {
        CHECK (clip->timelineStart == 2000);
        CHECK (clip->sourceOffset == 100);
        CHECK (clip->length == 3000);
        CHECK (std::abs (clip->gainDb) < 1e-12);
        CHECK (clip->fadeInLength == 0);
        CHECK (clip->fadeOutLength == 0);
    }
    CHECK (guitar[1].id != vocals[1].id);
    CHECK (passHeardStatus (*brought, "p2") == HeardStatus::heard);
    CHECK (brought->takes == song.takes);
}

TEST_CASE ("bringing back a Pass with no surviving Take, or an unknown Pass, is refused", "[song][take_lanes]")
{
    auto song = threePassSong();
    song.tracks.clear();
    const auto none = bringBackPass (song, "p1");
    REQUIRE (! none.has_value());
    CHECK (none.error().code == "song.pass_has_no_takes");
    CHECK (none.error().fields.at ("pass_id") == "p1");
    CHECK (bringBackPass (song, "nope").error().code == "song.pass_not_found");
}

TEST_CASE ("Swap and bring back are labelled after the Pass and the Track", "[song][take_lanes]")
{
    const auto song = threePassSong();
    const auto& g2 = song.takes[1];
    CHECK (useTakeLabel (song, g2) == "Use Pass 2 on Guitar");
    CHECK (bringBackTakeLabel (song, g2) == "Bring back Pass 2 on Guitar");
    CHECK (bringBackPassLabel (song, "p3") == "Bring back Pass 3");
}
