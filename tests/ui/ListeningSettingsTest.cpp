#include "ui/ListeningSettings.h"

#include "core/Log.h"

#include <catch2/catch_test_macros.hpp>

using overdub::core::ScopedLogCapture;
using overdub::song::Song;
using overdub::ui::withListeningSetting;
using namespace overdub::ui::intent;

// Transcribe mode's listening settings as Song → Song: no package, no OpenSong, no window. The Song
// has two Tracks, "track-bass" and "track-drums".
namespace
{
Song twoTracks()
{
    auto song = overdub::song::makeEmptySong (48000);
    song.tracks.push_back (overdub::song::Track { .id = "track-bass" });
    song.tracks.push_back (overdub::song::Track { .id = "track-drums" });
    return song;
}

Song changed (const Song& song, const ListeningSetting& setting)
{
    auto result = withListeningSetting (song, setting);
    REQUIRE (result.has_value());
    return *result;
}
} // namespace

TEST_CASE ("each listening setting changes only its own Track's setting", "[ui][listening_settings]")
{
    const auto song = twoTracks();

    CHECK (changed (song, SetFocusTrack { "track-bass" }).focusTrackId == "track-bass");
    CHECK (changed (song, SetTranscribeMute { "track-drums", true }).tracks[1].transcribeMute);
    CHECK (changed (song, SetTranscribeSolo { "track-drums", true }).tracks[1].transcribeSolo);
    CHECK (changed (song, SetOctaveShift { "track-bass", -1 }).tracks[0].octaveShift == -1);
    const auto filtered = changed (song, SetBandFilter { .trackId = "track-bass", .on = true, .lowHz = 200, .highHz = 3200 });
    CHECK (filtered.tracks[0].bandFilter == overdub::song::BandFilter { .on = true, .lowHz = 200, .highHz = 3200 });
    CHECK (filtered.tracks[1] == song.tracks[1]);
}

TEST_CASE ("a listening setting's check is the Song edit's", "[ui][listening_settings]")
{
    const auto song = twoTracks();

    CHECK (withListeningSetting (song, SetFocusTrack { "track-gone" }).error().code == "song.track_not_found");
    CHECK (withListeningSetting (song, SetOctaveShift { "track-bass", 3 }).error().code == "song.octave_shift_out_of_range");
    const auto inverted = withListeningSetting (song, SetBandFilter { .trackId = "track-bass", .on = true, .lowHz = 500, .highHz = 500 });
    CHECK (inverted.error().code == "song.band_filter_low_not_below_high");
}

TEST_CASE ("a stored listening setting logs its own event", "[ui][listening_settings]")
{
    ScopedLogCapture capture;

    overdub::ui::logListeningSet (SetOctaveShift { "track-bass", 2 });
    overdub::ui::logListeningSet (SetTranscribeSolo { "track-drums", false });

    const auto events = capture.events();
    REQUIRE (events.size() == 2);
    CHECK (events[0].at ("event") == "ui.octave_shift_set");
    CHECK (events[0].at ("track_id") == "track-bass");
    CHECK (events[0].at ("octaves") == 2);
    CHECK (events[1].at ("event") == "ui.transcribe_solo_set");
    CHECK (events[1].at ("solo") == false);
}
