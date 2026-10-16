#include "song/Song.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "song/Separation.h"
#include "song/TrackEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace overdub::song;

namespace
{
Song songWithGuitar()
{
    auto song = makeEmptySong (48000);
    song.tracks.push_back (Track { .id = "track-guitar", .name = "Guitar" });
    return song;
}

std::size_t firstChoice (std::size_t)
{
    return 0;
}

template <typename T>
overdub::core::Error errorOf (const overdub::core::Expected<T>& result)
{
    REQUIRE_FALSE (result.has_value());
    return result.error();
}
} // namespace

TEST_CASE ("a Track is looked up by its id", "[song][tracks][lookup]")
{
    auto song = songWithGuitar();
    const auto& constSong = song;

    CHECK (findTrack (constSong, "track-guitar")->name == "Guitar");
    CHECK (findTrack (constSong, "track-gone") == nullptr);
    REQUIRE (lookupTrack (constSong, "track-guitar").has_value());
    CHECK ((*lookupTrack (constSong, "track-guitar"))->name == "Guitar");

    (*lookupTrack (song, "track-guitar"))->name = "Lead";
    CHECK (song.tracks[0].name == "Lead");
    findTrack (song, "track-guitar")->name = "Rhythm";
    CHECK (song.tracks[0].name == "Rhythm");

    const auto missing = errorOf (lookupTrack (song, "track-gone"));
    CHECK (missing.code == "song.track_not_found");
    CHECK (missing.fields.at ("track_id") == "track-gone");
}

TEST_CASE ("every edit of a Track the Song does not have is song.track_not_found with its track_id", "[song][tracks][lookup]")
{
    const auto song = songWithGuitar();
    const std::vector<std::pair<std::string, overdub::core::Error>> refusals {
        { "setTrackVolume", errorOf (setTrackVolume (song, "track-gone", 0.0)) },
        { "setTrackPan", errorOf (setTrackPan (song, "track-gone", 0.0)) },
        { "setTrackMute", errorOf (setTrackMute (song, "track-gone", true)) },
        { "setTrackSolo", errorOf (setTrackSolo (song, "track-gone", true)) },
        { "deleteTrack", errorOf (deleteTrack (song, "track-gone")) },
        { "moveTrack", errorOf (moveTrack (song, "track-gone", 0)) },
        { "addInsert", errorOf (addInsert (song, "track-gone", InsertSlot { .id = "slot-1", .name = "AUDelay" })) },
        { "separableBackingTrack", errorOf (separableBackingTrack (song, "track-gone")) },
        { "withSeparation", errorOf (withSeparation (song, "track-gone", {}, firstChoice)) },
    };

    for (const auto& [edit, error] : refusals)
    {
        INFO (edit);
        CHECK (error.code == "song.track_not_found");
        CHECK (error.fields.at ("track_id") == "track-gone");
    }
}
