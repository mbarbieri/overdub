#include "song/ClipEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace overdub::song;

namespace
{
// One Track "t" holding Clips of one 10000-sample Take: "low" [0, 6000) reading the Take from 0,
// and "high" [2000, 5000) on top reading it from 1000.
Song twoLayerSong()
{
    Song song = makeEmptySong (48000);
    Take take;
    take.id = "take-1";
    take.lengthSamples = 10000;
    song.takes.push_back (take);

    Track track;
    track.id = "t";
    track.name = "Guitar";
    track.clips.push_back (Clip { .id = "low", .source = { ClipSourceKind::take, "take-1" }, .timelineStart = 0, .sourceOffset = 0, .length = 6000 });
    track.clips.push_back (Clip { .id = "high", .source = { ClipSourceKind::take, "take-1" }, .timelineStart = 2000, .sourceOffset = 1000, .length = 3000 });
    song.tracks.push_back (track);
    return song;
}

bool sameDb (double a, double b)
{
    return std::abs (a - b) < 1e-12;
}

const Clip& clipNamed (const Song& song, const Id& id)
{
    const auto* clip = findClip (song, id);
    REQUIRE (clip != nullptr);
    return *clip;
}
} // namespace

TEST_CASE ("moving a Clip changes only its timeline start and keeps its layer", "[song][clip_edits]")
{
    const auto before = twoLayerSong();
    const auto moved = moveClip (before, "low", 7000);
    REQUIRE (moved.has_value());

    auto expected = before;
    expected.tracks[0].clips[0].timelineStart = 7000;
    CHECK (*moved == expected);
    CHECK (moved->tracks[0].clips[0].id == "low"); // still the bottom layer
}

TEST_CASE ("a Clip cannot move before the Song's start", "[song][clip_edits]")
{
    const auto moved = moveClip (twoLayerSong(), "high", -500);
    REQUIRE (moved.has_value());
    CHECK (clipNamed (*moved, "high").timelineStart == 0);
}

TEST_CASE ("trimming a Clip's start moves its start and source offset together and keeps its end", "[song][clip_edits]")
{
    const auto trimmed = trimClipStart (twoLayerSong(), "high", 2600);
    REQUIRE (trimmed.has_value());
    const auto& clip = clipNamed (*trimmed, "high");
    CHECK (clip.timelineStart == 2600);
    CHECK (clip.sourceOffset == 1600);
    CHECK (clip.length == 2400);
    CHECK (trimmed->tracks[0].clips[1].id == "high");
}

TEST_CASE ("trimming a Clip's start stops at the source's first sample and one sample before its end", "[song][clip_edits]")
{
    // "high" reads the Take from 1000, so its start can come back at most 1000 samples, to 1000.
    const auto extended = trimClipStart (twoLayerSong(), "high", 0);
    REQUIRE (extended.has_value());
    CHECK (clipNamed (*extended, "high").timelineStart == 1000);
    CHECK (clipNamed (*extended, "high").sourceOffset == 0);
    CHECK (clipNamed (*extended, "high").length == 4000);

    const auto collapsed = trimClipStart (twoLayerSong(), "high", 9000);
    REQUIRE (collapsed.has_value());
    CHECK (clipNamed (*collapsed, "high").timelineStart == 4999);
    CHECK (clipNamed (*collapsed, "high").sourceOffset == 3999);
    CHECK (clipNamed (*collapsed, "high").length == 1);
}

TEST_CASE ("trimming a Clip's end changes its length within the source", "[song][clip_edits]")
{
    const auto shorter = trimClipEnd (twoLayerSong(), "high", 4000);
    REQUIRE (shorter.has_value());
    CHECK (clipNamed (*shorter, "high").length == 2000);
    CHECK (clipNamed (*shorter, "high").timelineStart == 2000);
    CHECK (clipNamed (*shorter, "high").sourceOffset == 1000);

    // The Take has 10000 samples and "high" reads from 1000: at most 9000 long, ending at 11000.
    const auto longest = trimClipEnd (twoLayerSong(), "high", 50000);
    REQUIRE (longest.has_value());
    CHECK (clipNamed (*longest, "high").length == 9000);

    const auto shortest = trimClipEnd (twoLayerSong(), "high", 100);
    REQUIRE (shortest.has_value());
    CHECK (clipNamed (*shortest, "high").length == 1);
}

TEST_CASE ("a trim keeps the Clip's fades inside its new length", "[song][clip_edits]")
{
    auto song = twoLayerSong();
    song.tracks[0].clips[1].fadeInLength = 1000;
    song.tracks[0].clips[1].fadeOutLength = 1500;

    const auto trimmed = trimClipEnd (song, "high", 3200); // 1200 long
    REQUIRE (trimmed.has_value());
    const auto& clip = clipNamed (*trimmed, "high");
    CHECK (clip.fadeInLength == 1000);
    CHECK (clip.fadeOutLength == 200);
}

TEST_CASE ("trimming a Clip shorter than its fades clamps them, fade-in first", "[song][clip_edits][clip_gain]")
{
    auto song = twoLayerSong();
    song.tracks[0].clips[1].fadeInLength = 1000; // "high" is [2000, 5000)
    song.tracks[0].clips[1].fadeOutLength = 1500;

    const auto endTrimmed = trimClipEnd (song, "high", 2300); // 300 long
    REQUIRE (endTrimmed.has_value());
    CHECK (clipNamed (*endTrimmed, "high").fadeInLength == 300);
    CHECK (clipNamed (*endTrimmed, "high").fadeOutLength == 0);

    const auto startTrimmed = trimClipStart (song, "high", 4600); // 400 long
    REQUIRE (startTrimmed.has_value());
    CHECK (clipNamed (*startTrimmed, "high").length == 400);
    CHECK (clipNamed (*startTrimmed, "high").fadeInLength == 400);
    CHECK (clipNamed (*startTrimmed, "high").fadeOutLength == 0);

    const auto oneSample = trimClipEnd (song, "high", 0);
    REQUIRE (oneSample.has_value());
    CHECK (clipNamed (*oneSample, "high").fadeInLength == 1);
    CHECK (clipNamed (*oneSample, "high").fadeOutLength == 0);
}

TEST_CASE ("a split zeroes the fades at the cut and clamps the others into each half", "[song][clip_edits][clip_gain]")
{
    auto song = twoLayerSong();
    song.tracks[0].clips[0].fadeInLength = 2000; // "low" is [0, 6000)
    song.tracks[0].clips[0].fadeOutLength = 3000;

    const auto split = splitClip (song, "low", 1500, "low-right");
    REQUIRE (split.has_value());
    CHECK (clipNamed (*split, "low").fadeInLength == 1500);
    CHECK (clipNamed (*split, "low").fadeOutLength == 0);
    CHECK (clipNamed (*split, "low-right").fadeInLength == 0);
    CHECK (clipNamed (*split, "low-right").fadeOutLength == 3000);
}

TEST_CASE ("setting a fade keeps fade-in plus fade-out within the Clip's length", "[song][clip_edits][clip_gain]")
{
    auto song = twoLayerSong();
    song.tracks[0].clips[1].fadeOutLength = 1000; // "high" is 3000 long

    const auto fadeIn = setClipFadeIn (song, "high", 1200);
    REQUIRE (fadeIn.has_value());
    CHECK (clipNamed (*fadeIn, "high").fadeInLength == 1200);
    CHECK (clipNamed (*fadeIn, "high").fadeOutLength == 1000);

    const auto tooLongIn = setClipFadeIn (song, "high", 2500);
    REQUIRE (tooLongIn.has_value());
    CHECK (clipNamed (*tooLongIn, "high").fadeInLength == 2000);

    const auto negativeIn = setClipFadeIn (song, "high", -50);
    REQUIRE (negativeIn.has_value());
    CHECK (clipNamed (*negativeIn, "high").fadeInLength == 0);

    const auto tooLongOut = setClipFadeOut (*fadeIn, "high", 5000);
    REQUIRE (tooLongOut.has_value());
    CHECK (clipNamed (*tooLongOut, "high").fadeOutLength == 1800);
    CHECK (clipNamed (*tooLongOut, "high").fadeInLength == 1200);

    const auto noOut = setClipFadeOut (song, "high", 0);
    REQUIRE (noOut.has_value());
    CHECK (clipNamed (*noOut, "high").fadeOutLength == 0);

    // Nothing else changes.
    auto expected = song;
    expected.tracks[0].clips[1].fadeInLength = 1200;
    CHECK (*fadeIn == expected);
}

TEST_CASE ("setting a Clip's gain keeps it between the limits", "[song][clip_edits][clip_gain]")
{
    const auto song = twoLayerSong();
    const auto quieter = setClipGain (song, "high", -7.5);
    REQUIRE (quieter.has_value());
    CHECK (sameDb (clipNamed (*quieter, "high").gainDb, -7.5));
    CHECK (sameDb (clipNamed (*quieter, "low").gainDb, 0.0));

    CHECK (sameDb (clipNamed (*setClipGain (song, "high", 40.0), "high").gainDb, 12.0));
    CHECK (sameDb (clipNamed (*setClipGain (song, "high", -100.0), "high").gainDb, -24.0));
    CHECK (sameDb (minClipGainDb, -24.0));
    CHECK (sameDb (maxClipGainDb, 12.0));
}

TEST_CASE ("fade and gain changes on an unknown Clip are refused", "[song][clip_edits][clip_gain]")
{
    const auto song = twoLayerSong();
    for (const auto& result : { setClipFadeIn (song, "gone", 10), setClipFadeOut (song, "gone", 10), setClipGain (song, "gone", 1.0) })
    {
        REQUIRE (! result.has_value());
        CHECK (result.error().code == "song.clip_not_found");
    }
}

TEST_CASE ("splitting a Clip makes two halves on the same layer that read the source continuously", "[song][clip_edits]")
{
    auto song = twoLayerSong();
    song.tracks[0].clips[0].fadeInLength = 100;
    song.tracks[0].clips[0].fadeOutLength = 200;
    song.tracks[0].clips[0].gainDb = -3.0;

    const auto split = splitClip (song, "low", 2500, "low-right");
    REQUIRE (split.has_value());
    const auto& clips = split->tracks[0].clips;
    REQUIRE (clips.size() == 3);

    // The halves take the split Clip's place in layer order: still under "high".
    CHECK (clips[0].id == "low");
    CHECK (clips[1].id == "low-right");
    CHECK (clips[2].id == "high");

    CHECK (clips[0].timelineStart == 0);
    CHECK (clips[0].sourceOffset == 0);
    CHECK (clips[0].length == 2500);
    CHECK (clips[0].fadeInLength == 100);
    CHECK (clips[0].fadeOutLength == 0);

    CHECK (clips[1].source == clips[0].source);
    CHECK (clips[1].timelineStart == 2500);
    CHECK (clips[1].sourceOffset == 2500);
    CHECK (clips[1].length == 3500);
    CHECK (clips[1].fadeInLength == 0);
    CHECK (clips[1].fadeOutLength == 200);
    CHECK (std::abs (clips[1].gainDb + 3.0) < 1e-12);
}

TEST_CASE ("a split outside the Clip, or on its edge, is refused", "[song][clip_edits]")
{
    for (const SampleCount at : { SampleCount { 2000 }, SampleCount { 5000 }, SampleCount { 1000 }, SampleCount { 7000 } })
    {
        INFO ("at " << at);
        const auto split = splitClip (twoLayerSong(), "high", at, "new");
        REQUIRE (! split.has_value());
        CHECK (split.error().code == "song.split_outside_clip");
    }
}

TEST_CASE ("deleting a Clip removes only it and keeps its source", "[song][clip_edits]")
{
    const auto before = twoLayerSong();
    const auto deleted = deleteClip (before, "low");
    REQUIRE (deleted.has_value());
    REQUIRE (deleted->tracks[0].clips.size() == 1);
    CHECK (deleted->tracks[0].clips[0] == before.tracks[0].clips[1]);
    CHECK (deleted->takes == before.takes);
}

TEST_CASE ("every Clip operation refuses an unknown Clip id", "[song][clip_edits]")
{
    const auto song = twoLayerSong();
    for (const auto& result : { moveClip (song, "nope", 0), trimClipStart (song, "nope", 0), trimClipEnd (song, "nope", 0), splitClip (song, "nope", 10, "x"), deleteClip (song, "nope") })
    {
        REQUIRE (! result.has_value());
        CHECK (result.error().code == "song.clip_not_found");
        CHECK (result.error().fields.at ("clip_id") == "nope");
    }
}

TEST_CASE ("a Clip's source length comes from its Take or Backing Track", "[song][clip_edits]")
{
    auto song = twoLayerSong();
    BackingTrack backing;
    backing.id = "backing-1";
    backing.lengthSamples = 777;
    song.backingTracks.push_back (backing);

    CHECK (sourceLength (song, ClipSource { ClipSourceKind::take, "take-1" }) == 10000);
    CHECK (sourceLength (song, ClipSource { ClipSourceKind::backing, "backing-1" }) == 777);
    CHECK (sourceLength (song, ClipSource { ClipSourceKind::take, "missing" }) == 0);
}

TEST_CASE ("covered spans are where a higher Clip hides a lower one", "[song][clip_edits]")
{
    auto song = twoLayerSong();
    CHECK (coveredSpans (song.tracks[0]) == std::vector<SampleSpan> { { 2000, 5000 } });

    // A third Clip on top overlapping "high" and past "low": spans merge where they touch.
    song.tracks[0].clips.push_back (Clip { .id = "top", .source = { ClipSourceKind::take, "take-1" }, .timelineStart = 4500, .sourceOffset = 0, .length = 3000 });
    CHECK (coveredSpans (song.tracks[0]) == std::vector<SampleSpan> { { 2000, 6000 } });

    song.tracks[0].clips = { song.tracks[0].clips[0] };
    CHECK (coveredSpans (song.tracks[0]).empty());
}

TEST_CASE ("trimming the end of a Clip whose source the Song lacks can shorten it but not lengthen it", "[song][clip_edits]")
{
    auto song = twoLayerSong();
    song.takes.clear();

    const auto shorter = trimClipEnd (song, "high", 4000);
    REQUIRE (shorter.has_value());
    CHECK (clipNamed (*shorter, "high").length == 2000);

    const auto longer = trimClipEnd (song, "high", 9000);
    REQUIRE (longer.has_value());
    CHECK (clipNamed (*longer, "high").length == 3000);
}
