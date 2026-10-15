#include "song/TrackEdits.h"

#include "song/TakeEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Adding an empty Track to record on (issue #109): named "Track N" with the lowest free N, Armed
// with the given input, and labelled for its undo step. Deleting a Track (issue #124): its Clips,
// mix and Insert Chain go; its Takes and Backing Tracks stay in the Song.
using namespace overdub::song;
using Catch::Matchers::WithinAbs;

TEST_CASE ("an added Track is empty, Armed on the given input and appended last", "[song][tracks]")
{
    auto song = makeEmptySong (48000);
    song.tracks.push_back (Track { .id = "b", .name = "Band Mix", .clips = { Clip { .id = "c" } } });

    const auto added = addTrack (song, { 0 });
    REQUIRE (added.song.tracks.size() == 2);
    CHECK (added.song.tracks[0] == song.tracks[0]);

    const auto& track = added.song.tracks[1];
    CHECK (track.id == added.trackId);
    CHECK (! track.id.empty());
    CHECK (track.name == "Track 1");
    CHECK (track.armed);
    CHECK (track.inputChannels == std::vector<int> { 0 });
    CHECK (track.clips.empty());
    CHECK (track.insertChain.empty());
    CHECK_THAT (track.volumeDb, WithinAbs (0.0, 0.0));
    CHECK_THAT (track.pan, WithinAbs (0.0, 0.0));
    CHECK (! track.mute);
    CHECK (! track.solo);
    CHECK (added.label == "Add Track 1");
}

TEST_CASE ("an added Track without an input is still Armed", "[song][tracks]")
{
    const auto added = addTrack (makeEmptySong (48000), {});
    CHECK (added.song.tracks[0].armed);
    CHECK (added.song.tracks[0].inputChannels.empty());
}

TEST_CASE ("an added Track takes the lowest Track number no other Track is named", "[song][tracks]")
{
    auto song = makeEmptySong (48000);
    CHECK (nextTrackName (song) == "Track 1");

    song.tracks.push_back (Track { .id = "1", .name = "Track 1" });
    song.tracks.push_back (Track { .id = "3", .name = "Track 3" });
    song.tracks.push_back (Track { .id = "b", .name = "Band Mix" });
    CHECK (nextTrackName (song) == "Track 2");

    const auto added = addTrack (song, {});
    CHECK (added.song.tracks.back().name == "Track 2");
    CHECK (added.label == "Add Track 2");
    CHECK (nextTrackName (added.song) == "Track 4");
}

TEST_CASE ("each added Track gets its own id", "[song][tracks]")
{
    const auto first = addTrack (makeEmptySong (48000), {});
    const auto second = addTrack (first.song, {});
    CHECK (second.song.tracks[0].id != second.song.tracks[1].id);
}

namespace
{
// Guitar and Bass, each with one Take of Pass "p1" under a Clip; Bass also plays Backing Track "bt".
Song twoTrackSongWithOnePass()
{
    auto song = makeEmptySong (48000);
    song.passes.push_back (Pass { .id = "p1" });
    song.takes.push_back (Take { .id = "tg", .trackId = "g", .passId = "p1", .lengthSamples = 100 });
    song.takes.push_back (Take { .id = "tb", .trackId = "b", .passId = "p1", .lengthSamples = 100 });
    song.backingTracks.push_back (BackingTrack { .id = "bt", .lengthSamples = 100 });
    song.tracks.push_back (Track { .id = "g", .name = "Guitar", .volumeDb = -3.0, .insertChain = { InsertSlot { .id = "s" } }, .clips = { Clip { .id = "cg", .source = { ClipSourceKind::take, "tg" }, .length = 100 } } });
    song.tracks.push_back (Track { .id = "b", .name = "Bass", .clips = { Clip { .id = "cb", .source = { ClipSourceKind::take, "tb" }, .length = 100 }, Clip { .id = "cbt", .source = { ClipSourceKind::backing, "bt" }, .length = 100 } } });
    return song;
}
} // namespace

TEST_CASE ("deleting a Track removes it with its Clips and keeps the Song's Takes and Backing Tracks", "[song][tracks]")
{
    const auto song = twoTrackSongWithOnePass();

    const auto deleted = deleteTrack (song, "b");

    REQUIRE (deleted.has_value());
    REQUIRE (deleted->song.tracks.size() == 1);
    CHECK (deleted->song.tracks[0] == song.tracks[0]);
    CHECK (deleted->song.takes == song.takes);
    CHECK (deleted->song.backingTracks == song.backingTracks);
    CHECK (deleted->song.passes == song.passes);
    CHECK (deleted->label == "Delete Track Bass");
}

TEST_CASE ("deleting the only Track leaves a Song with no Tracks", "[song][tracks]")
{
    auto song = makeEmptySong (48000);
    song.tracks.push_back (Track { .id = "g", .name = "Guitar", .armed = true });

    const auto deleted = deleteTrack (song, "g");

    REQUIRE (deleted.has_value());
    CHECK (deleted->song.tracks.empty());
}

TEST_CASE ("deleting a Track the Song does not have is song.track_not_found", "[song][tracks]")
{
    const auto deleted = deleteTrack (twoTrackSongWithOnePass(), "nope");

    REQUIRE_FALSE (deleted.has_value());
    CHECK (deleted.error().code == "song.track_not_found");
    CHECK (deleted.error().fields.at ("track_id") == "nope");
}

TEST_CASE ("after a Track is deleted, bringing back its Pass brings back only the Takes on remaining Tracks", "[song][tracks]")
{
    auto song = twoTrackSongWithOnePass();
    for (auto& track : song.tracks)
        std::erase_if (track.clips, [] (const Clip& clip) { return clip.source.kind == ClipSourceKind::take; });

    const auto deleted = deleteTrack (song, "b");
    REQUIRE (deleted.has_value());
    const auto broughtBack = bringBackPass (deleted->song, "p1");

    REQUIRE (broughtBack.has_value());
    REQUIRE (broughtBack->tracks.size() == 1);
    REQUIRE (broughtBack->tracks[0].clips.size() == 1);
    CHECK (broughtBack->tracks[0].clips[0].source == ClipSource { ClipSourceKind::take, "tg" });
}
