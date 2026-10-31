#include "ui/SongSettings.h"

#include "core/Log.h"
#include "song/LoopEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::exporting::ExportFormat;
using overdub::exporting::VideoLength;
using overdub::song::Song;
using overdub::ui::withSongSetting;
using namespace overdub::ui::intent;

// Every Song setting outside history as Song → Song: no package, no OpenSong, no window. The Song
// has two Tracks, "track-bass" and "track-drums", one Loop, "loop-1", and one Video Take, "video-1".
namespace
{
Song settingsSong()
{
    auto song = overdub::song::makeEmptySong (48000);
    song.tracks.push_back (overdub::song::Track { .id = "track-bass" });
    song.tracks.push_back (overdub::song::Track { .id = "track-drums" });
    auto withLoop = overdub::song::addLoop (song, 48000, 96000, "loop-1", 100);
    REQUIRE (withLoop.has_value());
    song = *withLoop;
    song.videoTakes.push_back (overdub::song::VideoTake { .id = "video-1" });
    return song;
}

Song changed (const Song& song, const SongSetting& setting)
{
    auto result = withSongSetting (song, setting);
    REQUIRE (result.has_value());
    return *result;
}
} // namespace

TEST_CASE ("arming and picking an input change only that Track, and an unknown Track changes nothing", "[ui][song_settings]")
{
    const auto song = settingsSong();

    const auto armed = changed (song, SetArmed { "track-bass", true });
    CHECK (armed.tracks[0].settings.armed);
    CHECK (armed.tracks[1] == song.tracks[1]);

    const auto stereo = changed (song, SetInputChannels { "track-drums", { 2, 3 } });
    CHECK (stereo.tracks[1].settings.inputChannels == std::vector<int> { 2, 3 });
    CHECK (stereo.tracks[0] == song.tracks[0]);

    CHECK (changed (song, SetArmed { "missing", true }) == song);
    CHECK (changed (song, SetInputChannels { "missing", { 0 } }) == song);
}

TEST_CASE ("pre-roll is stored as given", "[ui][song_settings]")
{
    CHECK (changed (settingsSong(), SetPreRoll { overdub::song::PreRoll::twoSeconds }).settings.preRoll == overdub::song::PreRoll::twoSeconds);
}

TEST_CASE ("looping needs an active Loop", "[ui][song_settings]")
{
    const auto song = settingsSong();

    CHECK (withSongSetting (song, SetLooping { true }).error().code == "song.no_active_loop");

    auto active = overdub::song::setActiveLoop (song, "loop-1", false);
    REQUIRE (active.has_value());
    CHECK (changed (*active, SetLooping { true }).settings.looping);
}

TEST_CASE ("each listening setting changes only its own Track's setting", "[ui][song_settings]")
{
    const auto song = settingsSong();

    CHECK (changed (song, SetFocusTrack { "track-bass" }).settings.focusTrackId == "track-bass");
    CHECK (changed (song, SetTranscribeMute { "track-drums", true }).tracks[1].settings.transcribeMute);
    CHECK (changed (song, SetTranscribeSolo { "track-drums", true }).tracks[1].settings.transcribeSolo);
    CHECK (changed (song, SetOctaveShift { "track-bass", -1 }).tracks[0].settings.octaveShift == -1);
    const auto filtered = changed (song, SetBandFilter { .trackId = "track-bass", .on = true, .lowHz = 200, .highHz = 3200 });
    CHECK (filtered.tracks[0].settings.bandFilter == overdub::song::BandFilter { .on = true, .lowHz = 200, .highHz = 3200 });
    CHECK (filtered.tracks[1] == song.tracks[1]);
}

TEST_CASE ("a listening setting's check is the Song edit's", "[ui][song_settings]")
{
    const auto song = settingsSong();

    CHECK (withSongSetting (song, SetFocusTrack { "track-gone" }).error().code == "song.track_not_found");
    CHECK (withSongSetting (song, SetOctaveShift { "track-bass", 3 }).error().code == "song.octave_shift_out_of_range");
    const auto inverted = withSongSetting (song, SetBandFilter { .trackId = "track-bass", .on = true, .lowHz = 500, .highHz = 500 });
    CHECK (inverted.error().code == "song.band_filter_low_not_below_high");
}

TEST_CASE ("an export format is stored as given", "[ui][song_settings]")
{
    const auto song = settingsSong();

    CHECK (changed (song, SetExportFormat { ExportFormat::m4a }).settings.exportFormat == ExportFormat::m4a);
    CHECK (changed (song, SetExportFormat { ExportFormat::mp4 }).settings.exportFormat == ExportFormat::mp4);
}

TEST_CASE ("an export length stores the MP4 length and the Loop whose range is written", "[ui][song_settings]")
{
    const auto song = settingsSong();

    const auto ofLoop = changed (song, SetExportLength { VideoLength::fullSong, "loop-1" });
    CHECK (ofLoop.settings.exportLength == VideoLength::fullSong);
    CHECK (ofLoop.settings.exportLoopId == "loop-1");

    const auto whole = changed (ofLoop, SetExportLength { VideoLength::trimToVideoTake, std::nullopt });
    CHECK (whole.settings.exportLength == VideoLength::trimToVideoTake);
    CHECK_FALSE (whole.settings.exportLoopId.has_value());
}

TEST_CASE ("an export length of a Loop the Song does not have is song.loop_not_found", "[ui][song_settings]")
{
    const auto result = withSongSetting (settingsSong(), SetExportLength { VideoLength::fullSong, "loop-gone" });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.loop_not_found");
    CHECK (result.error().fields.at ("loop_id") == "loop-gone");
}

TEST_CASE ("an export destination in an existing folder is stored as its full path", "[ui][song_settings]")
{
    const juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory().wasOk());
    const auto file = folder.getFile().getChildFile ("take one.wav");

    const auto song = changed (settingsSong(), SetExportDestination { { file.getFullPathName().toStdString() } });

    CHECK (song.settings.exportDestination == file.getFullPathName().toStdString());
    folder.getFile().deleteRecursively();
}

TEST_CASE ("an export destination whose folder does not exist is export.folder_missing", "[ui][song_settings]")
{
    const juce::TemporaryFile missing;
    const auto file = missing.getFile().getChildFile ("song.wav");
    REQUIRE_FALSE (missing.getFile().exists());

    const auto result = withSongSetting (settingsSong(), SetExportDestination { { file.getFullPathName().toStdString() } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "export.folder_missing");
}

TEST_CASE ("an export Video Take of the Song is stored, and any other id is ignored", "[ui][song_settings]")
{
    const auto song = settingsSong();

    CHECK (changed (song, SetExportVideoTake { "video-1" }).settings.exportVideoTakeId == "video-1");
    CHECK (changed (song, SetExportVideoTake { "video-gone" }) == song);
}

TEST_CASE ("a stored setting logs its own event, and the Record and Export settings log none", "[ui][song_settings]")
{
    ScopedLogCapture capture;

    overdub::ui::logSongSettingSet (SetOctaveShift { "track-bass", 2 });
    overdub::ui::logSongSettingSet (SetTranscribeSolo { "track-drums", false });
    overdub::ui::logSongSettingSet (SetLooping { true });
    overdub::ui::logSongSettingSet (SetArmed { "track-bass", true });
    overdub::ui::logSongSettingSet (SetExportFormat { ExportFormat::wav });

    const auto events = capture.events();
    REQUIRE (events.size() == 3);
    CHECK (events[0].at ("event") == "ui.octave_shift_set");
    CHECK (events[0].at ("track_id") == "track-bass");
    CHECK (events[0].at ("octaves") == 2);
    CHECK (events[1].at ("event") == "ui.transcribe_solo_set");
    CHECK (events[1].at ("solo") == false);
    CHECK (events[2].at ("event") == "ui.looping_set");
    CHECK (events[2].at ("looping") == true);
}
