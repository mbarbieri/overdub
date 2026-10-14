#include "song/TrackEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Adding an empty Track to record on (issue #109): named "Track N" with the lowest free N, Armed
// with the given input, and labelled for its undo step.
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
