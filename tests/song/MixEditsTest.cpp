#include "song/MixEdits.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// The Mixer's Song operations (issue #31): each changes one setting of one Track (or the Master),
// clamped to its range, and names its undo step.
using namespace overdub::song;
using Catch::Matchers::WithinAbs;

namespace
{
Song twoTracks()
{
    auto song = makeEmptySong (48000);
    song.tracks.push_back (Track { .id = "g", .name = "Guitar" });
    song.tracks.push_back (Track { .id = "v", .name = "Vocals" });
    return song;
}
} // namespace

TEST_CASE ("a Track's volume is set in dB within the fader's range", "[song][mixer]")
{
    const auto song = twoTracks();
    CHECK_THAT (setTrackVolume (song, "g", -6.5)->tracks[0].volumeDb, WithinAbs (-6.5, 0.0));
    CHECK (setTrackVolume (song, "g", -6.5)->tracks[1] == song.tracks[1]);
    CHECK_THAT (setTrackVolume (song, "v", -200.0)->tracks[1].volumeDb, WithinAbs (minVolumeDb, 0.0));
    CHECK_THAT (setTrackVolume (song, "v", 40.0)->tracks[1].volumeDb, WithinAbs (maxVolumeDb, 0.0));
}

TEST_CASE ("a Track's pan is set within hard left and hard right", "[song][mixer]")
{
    const auto song = twoTracks();
    CHECK_THAT (setTrackPan (song, "v", 0.25)->tracks[1].pan, WithinAbs (0.25, 0.0));
    CHECK_THAT (setTrackPan (song, "v", -3.0)->tracks[1].pan, WithinAbs (-1.0, 0.0));
    CHECK_THAT (setTrackPan (song, "v", 3.0)->tracks[1].pan, WithinAbs (1.0, 0.0));
}

TEST_CASE ("mute and solo are set per Track", "[song][mixer]")
{
    auto song = twoTracks();
    song = *setTrackMute (song, "g", true);
    song = *setTrackSolo (song, "v", true);
    CHECK (song.tracks[0].mute);
    CHECK (! song.tracks[0].solo);
    CHECK (song.tracks[1].solo);
    CHECK (! song.tracks[1].mute);
    CHECK (! setTrackMute (song, "g", false)->tracks[0].mute);
}

TEST_CASE ("the Master volume is set in dB within the fader's range", "[song][mixer]")
{
    const auto song = twoTracks();
    CHECK_THAT (setMasterVolume (song, 3.0).master.volumeDb, WithinAbs (3.0, 0.0));
    CHECK_THAT (setMasterVolume (song, -100.0).master.volumeDb, WithinAbs (minVolumeDb, 0.0));
}

TEST_CASE ("a Mixer change to a Track the Song does not have is song.track_not_found", "[song][mixer]")
{
    const auto song = twoTracks();
    for (const auto& result : { setTrackVolume (song, "x", 0.0), setTrackPan (song, "x", 0.0), setTrackMute (song, "x", true), setTrackSolo (song, "x", true) })
    {
        REQUIRE (! result.has_value());
        CHECK (result.error().code == "song.track_not_found");
        CHECK (result.error().fields.at ("track_id") == "x");
    }
}

TEST_CASE ("Mixer undo steps are named after the Track and the change", "[song][mixer]")
{
    const auto song = twoTracks();
    CHECK (volumeLabel (song, "g") == "Guitar Volume");
    CHECK (panLabel (song, "v") == "Vocals Pan");
    CHECK (muteLabel (song, "g", true) == "Mute Guitar");
    CHECK (muteLabel (song, "g", false) == "Unmute Guitar");
    CHECK (soloLabel (song, "v", true) == "Solo Vocals");
    CHECK (soloLabel (song, "v", false) == "Unsolo Vocals");
    CHECK (std::string (masterVolumeLabel) == "Master Volume");
}

TEST_CASE ("two Songs that differ only in their mix are told apart from any other change", "[song][mixer]")
{
    const auto song = twoTracks();
    auto mixed = *setTrackVolume (song, "g", -3.0);
    mixed = *setTrackPan (mixed, "v", 0.5);
    mixed = *setTrackMute (mixed, "v", true);
    mixed = *setTrackSolo (mixed, "g", true);
    mixed = setMasterVolume (mixed, -2.0);
    CHECK (differsOnlyInMix (song, mixed));
    CHECK (differsOnlyInMix (song, song));

    auto renamed = mixed;
    renamed.tracks[0].name = "Bass";
    CHECK (! differsOnlyInMix (song, renamed));

    auto withClip = mixed;
    withClip.tracks[1].clips.push_back (Clip { .id = "c" });
    CHECK (! differsOnlyInMix (song, withClip));

    auto reordered = song;
    std::swap (reordered.tracks[0], reordered.tracks[1]);
    CHECK (! differsOnlyInMix (song, reordered));
}
