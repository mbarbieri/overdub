#include "song/TrackEdits.h"

#include "song/TakeEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Adding an empty Track to record on (issue #109): named "Track N" with the lowest free N, Armed
// with the given input, and labelled for its undo step. Deleting a Track (issue #124): its Clips,
// mix and Insert Chain go; its Takes and Backing Tracks stay in the Song. Moving a Track (issue
// #138) changes only the Track order.
using namespace overdub::song;
using Catch::Matchers::WithinAbs;

namespace
{
// A colour pick that always takes the first choice.
std::size_t firstChoice (std::size_t)
{
    return 0;
}
} // namespace

TEST_CASE ("an added Track is empty, Armed on the given input and appended last", "[song][tracks]")
{
    auto song = makeEmptySong (48000);
    song.tracks.push_back (Track { .id = "b", .name = "Band Mix", .clips = { Clip { .id = "c" } } });

    const auto added = addTrack (song, { 0 }, firstChoice);
    REQUIRE (added.song.tracks.size() == 2);
    CHECK (added.song.tracks[0] == song.tracks[0]);

    const auto& track = added.song.tracks[1];
    CHECK (track.id == added.trackId);
    CHECK (! track.id.empty());
    CHECK (track.name == "Track 1");
    CHECK (track.settings.armed);
    CHECK (track.settings.inputChannels == std::vector<int> { 0 });
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
    const auto added = addTrack (makeEmptySong (48000), {}, firstChoice);
    CHECK (added.song.tracks[0].settings.armed);
    CHECK (added.song.tracks[0].settings.inputChannels.empty());
}

TEST_CASE ("an added Track takes the lowest Track number no other Track is named", "[song][tracks]")
{
    auto song = makeEmptySong (48000);
    CHECK (nextTrackName (song) == "Track 1");

    song.tracks.push_back (Track { .id = "1", .name = "Track 1" });
    song.tracks.push_back (Track { .id = "3", .name = "Track 3" });
    song.tracks.push_back (Track { .id = "b", .name = "Band Mix" });
    CHECK (nextTrackName (song) == "Track 2");

    const auto added = addTrack (song, {}, firstChoice);
    CHECK (added.song.tracks.back().name == "Track 2");
    CHECK (added.label == "Add Track 2");
    CHECK (nextTrackName (added.song) == "Track 4");
}

TEST_CASE ("each added Track gets its own id", "[song][tracks]")
{
    const auto first = addTrack (makeEmptySong (48000), {}, firstChoice);
    const auto second = addTrack (first.song, {}, firstChoice);
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
    song.tracks.push_back (Track { .id = "g", .name = "Guitar", .settings = { .armed = true } });

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

namespace
{
// Four Tracks a, b, c, d with their own Armed, mute, solo, inputs and Clips, and one Pass.
Song fourTrackSong()
{
    auto song = twoTrackSongWithOnePass();
    song.tracks[0].id = "a";
    song.tracks[0].settings.armed = true;
    song.tracks[0].settings.inputChannels = { 0 };
    song.tracks[1].id = "b";
    song.tracks[1].mute = true;
    song.tracks.push_back (Track { .id = "c", .name = "Keys", .solo = true, .settings = { .inputChannels = { 1, 2 } } });
    song.tracks.push_back (Track { .id = "d", .name = "Drums", .colour = "#aabbcc", .settings = { .armed = true } });
    song.settings.preRoll = PreRoll::twoSeconds;
    song.settings.playhead = 4800;
    return song;
}

std::vector<Id> trackOrder (const Song& song)
{
    std::vector<Id> ids;
    for (const auto& track : song.tracks)
        ids.push_back (track.id);
    return ids;
}
} // namespace

TEST_CASE ("moving a Track puts it at the target index and keeps the others in order", "[song][tracks][move_track]")
{
    const auto song = fourTrackSong();
    const auto order = [&song] (const Id& trackId, std::size_t toIndex) {
        auto moved = moveTrack (song, trackId, toIndex);
        REQUIRE (moved.has_value());
        REQUIRE (moved->has_value());
        CHECK ((*moved)->label == "Move Track");
        return trackOrder ((*moved)->song);
    };

    CHECK (order ("b", 0) == std::vector<Id> { "b", "a", "c", "d" });
    CHECK (order ("b", 2) == std::vector<Id> { "a", "c", "b", "d" });
    CHECK (order ("b", 3) == std::vector<Id> { "a", "c", "d", "b" });
    CHECK (order ("a", 3) == std::vector<Id> { "b", "c", "d", "a" });
    CHECK (order ("d", 0) == std::vector<Id> { "d", "a", "b", "c" });
    CHECK (order ("c", 1) == std::vector<Id> { "a", "c", "b", "d" });
    CHECK (order ("a", 99) == std::vector<Id> { "b", "c", "d", "a" }); // clamped to the last place
}

TEST_CASE ("moving a Track to any other index changes nothing but the Track order", "[song][tracks][move_track]")
{
    const auto song = fourTrackSong();
    for (std::size_t from = 0; from < song.tracks.size(); ++from)
    {
        for (std::size_t to = 0; to < song.tracks.size(); ++to)
        {
            if (to == from)
                continue;
            const auto moved = moveTrack (song, song.tracks[from].id, to);
            REQUIRE (moved.has_value());
            REQUIRE (moved->has_value());
            const auto& result = (*moved)->song;

            REQUIRE (result.tracks.size() == song.tracks.size());
            CHECK (result.tracks[to] == song.tracks[from]);
            auto others = song.tracks;
            others.erase (others.begin() + (std::ptrdiff_t) from);
            auto resultOthers = result.tracks;
            resultOthers.erase (resultOthers.begin() + (std::ptrdiff_t) to);
            CHECK (resultOthers == others);

            auto restored = result;
            restored.tracks = song.tracks;
            CHECK (restored == song);
        }
    }
}

TEST_CASE ("moving a Track to its own index is a no-op with no undo step", "[song][tracks][move_track]")
{
    const auto song = fourTrackSong();
    for (std::size_t index = 0; index < song.tracks.size(); ++index)
    {
        const auto moved = moveTrack (song, song.tracks[index].id, index);
        REQUIRE (moved.has_value());
        CHECK_FALSE (moved->has_value());
    }
    const auto lastPastTheEnd = moveTrack (song, "d", 7);
    REQUIRE (lastPastTheEnd.has_value());
    CHECK_FALSE (lastPastTheEnd->has_value());
}

TEST_CASE ("moving a Track the Song does not have is song.track_not_found", "[song][tracks][move_track]")
{
    const auto moved = moveTrack (fourTrackSong(), "nope", 0);

    REQUIRE_FALSE (moved.has_value());
    CHECK (moved.error().code == "song.track_not_found");
    CHECK (moved.error().fields.at ("track_id") == "nope");
}
