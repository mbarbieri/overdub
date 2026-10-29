#include "song/TranscribeMix.h"

#include "song/MixEdits.h"
#include "song/TrackEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace overdub::song;

// The Focus Track and the Transcribe Mix (issue #226, ADR 0010) as pure Song functions: picking a
// Focus Track solos it alone, the Transcribe Mix replaces only mute and solo, and both live outside
// history.
namespace
{
Song threeTracks()
{
    auto song = makeEmptySong (48000);
    song.tracks = {
        Track { .id = "track-band", .name = "Band", .volumeDb = -6.0 },
        Track { .id = "track-guitar", .name = "Guitar", .pan = 0.5 },
        Track { .id = "track-vocals", .name = "Vocals" },
    };
    return song;
}

std::vector<bool> transcribeSolos (const Song& song)
{
    std::vector<bool> solos;
    for (const auto& track : song.tracks)
        solos.push_back (track.transcribeSolo);
    return solos;
}

std::vector<bool> transcribeMutes (const Song& song)
{
    std::vector<bool> mutes;
    for (const auto& track : song.tracks)
        mutes.push_back (track.transcribeMute);
    return mutes;
}

std::size_t firstColour (std::size_t)
{
    return 0;
}
} // namespace

TEST_CASE ("picking a Focus Track solos it in the Transcribe Mix and unsolos every other Track; mutes stay", "[song][transcribe][focus]")
{
    auto song = threeTracks();
    song.tracks[0].transcribeSolo = true;
    song.tracks[2].transcribeMute = true;

    const auto focused = setFocusTrack (song, "track-guitar");

    REQUIRE (focused.has_value());
    CHECK (focused->focusTrackId == "track-guitar");
    CHECK (focusTrack (*focused)->name == "Guitar");
    CHECK (transcribeSolos (*focused) == std::vector { false, true, false });
    CHECK (transcribeMutes (*focused) == std::vector { false, false, true });
    // The Song's own mix never changes.
    for (std::size_t i = 0; i < song.tracks.size(); ++i)
    {
        CHECK (focused->tracks[i].mute == song.tracks[i].mute);
        CHECK (focused->tracks[i].solo == song.tracks[i].solo);
    }
}

TEST_CASE ("clearing the Focus Track leaves the Transcribe Mix as it is", "[song][transcribe][focus]")
{
    auto song = setFocusTrack (threeTracks(), "track-vocals").value();
    song = setTranscribeMute (song, "track-band", true).value();

    const auto cleared = setFocusTrack (song, std::nullopt);

    REQUIRE (cleared.has_value());
    CHECK_FALSE (cleared->focusTrackId.has_value());
    CHECK (focusTrack (*cleared) == nullptr);
    CHECK (transcribeSolos (*cleared) == std::vector { false, false, true });
    CHECK (transcribeMutes (*cleared) == std::vector { true, false, false });
}

TEST_CASE ("the Focus Track, Transcribe mute and solo of a Track the Song does not have are song.track_not_found", "[song][transcribe][lookup]")
{
    const auto song = threeTracks();
    for (const auto& result : { setFocusTrack (song, "track-gone"), setTranscribeMute (song, "track-gone", true), setTranscribeSolo (song, "track-gone", true) })
    {
        REQUIRE_FALSE (result.has_value());
        CHECK (result.error().code == "song.track_not_found");
        CHECK (result.error().fields.at ("track_id") == "track-gone");
    }
}

TEST_CASE ("the Song Transcribe mode hears takes mute and solo from the Transcribe Mix and everything else from the Song", "[song][transcribe][mix]")
{
    auto song = threeTracks();
    song.tracks[0].mute = true;
    song.tracks[1].solo = true;
    song.tracks[1].transcribeMute = true;
    song.tracks[2].transcribeSolo = true;

    const auto heard = withTranscribeMix (song);

    CHECK (heard.tracks[0].mute == false);
    CHECK (heard.tracks[0].solo == false);
    CHECK (heard.tracks[1].mute == true);
    CHECK (heard.tracks[1].solo == false);
    CHECK (heard.tracks[2].mute == false);
    CHECK (heard.tracks[2].solo == true);
    CHECK (heard.tracks[0].volumeDb == song.tracks[0].volumeDb);
    CHECK (heard.tracks[1].pan == song.tracks[1].pan);
    CHECK (differsOnlyInMix (song, heard));
}

TEST_CASE ("deleting the Focus Track clears it; deleting another Track keeps it", "[song][transcribe][focus]")
{
    const auto song = setFocusTrack (threeTracks(), "track-guitar").value();

    CHECK (deleteTrack (song, "track-vocals")->song.focusTrackId == "track-guitar");
    CHECK_FALSE (deleteTrack (song, "track-guitar")->song.focusTrackId.has_value());
}

TEST_CASE ("a new Track joins the Transcribe Mix unmuted and unsoloed", "[song][transcribe][mix]")
{
    auto song = setFocusTrack (threeTracks(), "track-guitar").value();
    song = setTranscribeMute (song, "track-band", true).value();

    const auto added = addTrack (song, {}, firstColour);

    const auto* track = findTrack (added.song, added.trackId);
    REQUIRE (track != nullptr);
    CHECK_FALSE (track->transcribeMute);
    CHECK_FALSE (track->transcribeSolo);
    CHECK (added.song.focusTrackId == "track-guitar");
}

TEST_CASE ("undo and redo keep the Focus Track and the Transcribe Mix as they are now", "[song][transcribe][history]")
{
    const auto snapshot = threeTracks();
    auto current = setFocusTrack (snapshot, "track-guitar").value();
    current = setTranscribeMute (current, "track-band", true).value();
    current.tracks[2].name = "Lead Vocals";

    const auto restored = keepStateOutsideHistory (snapshot, current);

    CHECK (restored.tracks[2].name == "Vocals");
    CHECK (restored.focusTrackId == "track-guitar");
    CHECK (transcribeSolos (restored) == std::vector { false, true, false });
    CHECK (transcribeMutes (restored) == std::vector { true, false, false });

    SECTION ("a Focus Track the snapshot does not have is cleared")
    {
        auto withoutGuitar = snapshot;
        withoutGuitar.tracks.erase (withoutGuitar.tracks.begin() + 1);
        CHECK_FALSE (keepStateOutsideHistory (withoutGuitar, current).focusTrackId.has_value());
    }
}

TEST_CASE ("each Track keeps its own octave shift from -1 to +2; the Focus Track's is the one heard", "[song][transcribe][octave]")
{
    auto song = setFocusTrack (threeTracks(), "track-guitar").value();
    CHECK (focusOctaveShift (song) == 0);

    song = setOctaveShift (song, "track-guitar", 1).value();
    song = setOctaveShift (song, "track-band", -1).value();

    CHECK (findTrack (song, "track-guitar")->octaveShift == 1);
    CHECK (findTrack (song, "track-band")->octaveShift == -1);
    CHECK (focusOctaveShift (song) == 1);
    CHECK (focusOctaveShift (setFocusTrack (song, "track-band").value()) == -1);
    CHECK (focusOctaveShift (setFocusTrack (song, "track-vocals").value()) == 0);
    CHECK (focusOctaveShift (setFocusTrack (song, std::nullopt).value()) == 0);
    // The Transcribe Mix's own mute and solo, and the Song's mix, are untouched.
    CHECK (transcribeSolos (song) == std::vector { false, true, false });
    CHECK_FALSE (findTrack (song, "track-guitar")->solo);

    for (const auto octaves : { -2, 3 })
    {
        const auto refused = setOctaveShift (song, "track-guitar", octaves);
        REQUIRE_FALSE (refused.has_value());
        CHECK (refused.error().code == "song.octave_shift_out_of_range");
        CHECK (refused.error().fields.at ("octaves") == octaves);
        CHECK (refused.error().fields.at ("min") == -1);
        CHECK (refused.error().fields.at ("max") == 2);
    }
    CHECK (setOctaveShift (song, "track-gone", 1).error().code == "song.track_not_found");
}

TEST_CASE ("undo and redo keep every Track's octave shift as it is now", "[song][transcribe][octave][history]")
{
    const auto snapshot = threeTracks();
    auto current = setOctaveShift (snapshot, "track-guitar", 2).value();
    current = setOctaveShift (current, "track-band", -1).value();

    const auto restored = keepStateOutsideHistory (snapshot, current);

    CHECK (findTrack (restored, "track-guitar")->octaveShift == 2);
    CHECK (findTrack (restored, "track-band")->octaveShift == -1);
    CHECK (findTrack (restored, "track-vocals")->octaveShift == 0);
}

TEST_CASE ("each Track keeps its own band filter, its low cut below its high cut within 20..20000 Hz, and undo keeps it", "[song][transcribe][band_filter]")
{
    const auto snapshot = threeTracks();
    const BandFilter guitar { .on = true, .lowHz = 200, .highHz = 3200 };
    auto current = setBandFilter (snapshot, "track-guitar", guitar).value();
    current = setBandFilter (current, "track-band", { .on = false, .lowHz = 20, .highHz = 20000 }).value();

    CHECK (findTrack (current, "track-guitar")->bandFilter == guitar);
    CHECK (findTrack (current, "track-vocals")->bandFilter == BandFilter {});
    CHECK (findTrack (current, "track-guitar")->octaveShift == 0);

    const auto restored = keepStateOutsideHistory (snapshot, current);
    CHECK (findTrack (restored, "track-guitar")->bandFilter == guitar);
    CHECK (findTrack (restored, "track-band")->bandFilter == BandFilter { .on = false, .lowHz = 20, .highHz = 20000 });

    const auto equal = setBandFilter (current, "track-guitar", { .on = true, .lowHz = 500, .highHz = 500 });
    REQUIRE_FALSE (equal.has_value());
    CHECK (equal.error().code == "song.band_filter_low_not_below_high");
    CHECK (equal.error().fields.at ("low_hz") == 500);
    const auto tooLow = setBandFilter (current, "track-guitar", { .on = true, .lowHz = 19, .highHz = 500 });
    REQUIRE_FALSE (tooLow.has_value());
    CHECK (tooLow.error().code == "song.band_filter_out_of_range");
    CHECK (tooLow.error().fields.at ("min_hz") == 20);
    CHECK (tooLow.error().fields.at ("max_hz") == 20000);
    CHECK (setBandFilter (current, "track-guitar", { .lowHz = 200, .highHz = 20001 }).error().code == "song.band_filter_out_of_range");
    CHECK (setBandFilter (current, "track-gone", guitar).error().code == "song.track_not_found");
}
