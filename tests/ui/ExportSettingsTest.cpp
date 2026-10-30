#include "ui/ExportSettings.h"

#include "song/LoopEdits.h"

#include <catch2/catch_test_macros.hpp>

using overdub::exporting::ExportFormat;
using overdub::exporting::VideoLength;
using overdub::song::Song;
using overdub::ui::withExportSetting;
using namespace overdub::ui::intent;

// The Export sheet's choices as Song → Song (issue #250): no package, no OpenSong, no window. The
// Song has one Loop, "loop-1", and one Video Take, "video-1".
namespace
{
Song exportSong()
{
    auto song = overdub::song::makeEmptySong (48000);
    auto withLoop = overdub::song::addLoop (song, 48000, 96000, "loop-1", 100);
    REQUIRE (withLoop.has_value());
    song = *withLoop;
    song.videoTakes.push_back (overdub::song::VideoTake { .id = "video-1" });
    return song;
}

Song changed (const Song& song, const overdub::ui::ExportSetting& setting)
{
    auto result = withExportSetting (song, setting);
    REQUIRE (result.has_value());
    return *result;
}
} // namespace

TEST_CASE ("an export format is stored as given", "[ui][export_settings]")
{
    const auto song = exportSong();

    CHECK (changed (song, SetExportFormat { ExportFormat::m4a }).exportFormat == ExportFormat::m4a);
    CHECK (changed (song, SetExportFormat { ExportFormat::mp4 }).exportFormat == ExportFormat::mp4);
}

TEST_CASE ("an export length stores the MP4 length and the Loop whose range is written", "[ui][export_settings]")
{
    const auto song = exportSong();

    const auto ofLoop = changed (song, SetExportLength { VideoLength::fullSong, "loop-1" });
    CHECK (ofLoop.exportLength == VideoLength::fullSong);
    CHECK (ofLoop.exportLoopId == "loop-1");

    const auto whole = changed (ofLoop, SetExportLength { VideoLength::trimToVideoTake, std::nullopt });
    CHECK (whole.exportLength == VideoLength::trimToVideoTake);
    CHECK_FALSE (whole.exportLoopId.has_value());
}

TEST_CASE ("an export length of a Loop the Song does not have is song.loop_not_found", "[ui][export_settings]")
{
    const auto result = withExportSetting (exportSong(), SetExportLength { VideoLength::fullSong, "loop-gone" });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.loop_not_found");
    CHECK (result.error().fields.at ("loop_id") == "loop-gone");
}

TEST_CASE ("an export destination in an existing folder is stored as its full path", "[ui][export_settings]")
{
    const juce::TemporaryFile folder;
    REQUIRE (folder.getFile().createDirectory().wasOk());
    const auto file = folder.getFile().getChildFile ("take one.wav");

    const auto song = changed (exportSong(), SetExportDestination { { file.getFullPathName().toStdString() } });

    CHECK (song.exportDestination == file.getFullPathName().toStdString());
    folder.getFile().deleteRecursively();
}

TEST_CASE ("an export destination whose folder does not exist is export.folder_missing", "[ui][export_settings]")
{
    const juce::TemporaryFile missing;
    const auto file = missing.getFile().getChildFile ("song.wav");
    REQUIRE_FALSE (missing.getFile().exists());

    const auto result = withExportSetting (exportSong(), SetExportDestination { { file.getFullPathName().toStdString() } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "export.folder_missing");
}

TEST_CASE ("an export Video Take of the Song is stored, and any other id is ignored", "[ui][export_settings]")
{
    const auto song = exportSong();

    CHECK (changed (song, SetExportVideoTake { "video-1" }).exportVideoTakeId == "video-1");
    CHECK (changed (song, SetExportVideoTake { "video-gone" }) == song);
}
