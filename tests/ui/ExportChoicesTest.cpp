#include "ui/ExportSheet.h"

#include "export/MixRender.h"
#include "song/AudioFileDecode.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// The Song remembers how it was exported (issue #201): set_export_format, set_export_length and
// set_export_destination store their value in the Song outside history, the Export sheet opens on
// them after the Song is reopened, says when Export replaces a file, and falls back to the export
// folder when the stored destination's folder is gone.
using overdub::core::ScopedLogCapture;
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportRequest;
using overdub::exporting::VideoLength;
using overdub::ui::Origin;
using overdub::ui::SongIntent;
using overdub::ui::intent::Export;
using overdub::ui::intent::SetExportFormat;
using overdub::ui::intent::SetExportLength;
using namespace overdub::ui::testing;

namespace
{
std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}

// A fresh folder for export files, deleted at the end of the test.
struct ExportFolder
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-export-choices-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};

// Closes the window's Song, which writes song.json, and opens its package again.
void reopenSong (MainWindowFixture& f)
{
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    auto reopened = overdub::song::SongPackage::open (location);
    REQUIRE (reopened.has_value());
    f.openSong = std::make_unique<overdub::ui::OpenSong> (std::move (reopened->package), overdub::ui::AudioBinding { &f.engine, {}, { "In 1", "In 2" } });
    f.window->setOpenSong (f.openSong.get());
}

std::string songName (MainWindowFixture& f)
{
    return f.openSong->package().location().getFileNameWithoutExtension().toStdString();
}

void tickUntilExported (MainWindowFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
}

void run (MainWindowFixture& f, overdub::ui::SongIntent intent)
{
    REQUIRE (f.window->run (std::move (intent), Origin::control).has_value());
}
} // namespace

TEST_CASE ("a Song with nothing saved opens the Export sheet on WAV, trimmed to the Video Take, into <export folder>/<Song name>.wav", "[ui][export][choices]")
{
    ExportFolder folder;
    MainWindowFixture f;
    f.window->exportFolder = folder.root;
    REQUIRE (f.song().settings.exportDestination.empty());

    auto& sheet = openExportSheet (*f.window);
    CHECK (sheet.wavButton.getToggleState());
    CHECK (sheet.request() == ExportRequest { ExportFormat::wav, folder.root.getChildFile (songName (f) + ".wav"), std::nullopt, VideoLength::trimToVideoTake });
    CHECK (sheet.destinationLabel.getText() == folder.root.getChildFile (songName (f) + ".wav").getFullPathName());
    CHECK (! isShown (sheet.replacesLabel));
}

TEST_CASE ("the format, the MP4 length and the destination are saved with the Song outside history, and the sheet shows them after the Song is reopened", "[ui][export][choices]")
{
    ExportFolder folder;
    MainWindowFixture f;
    const auto canUndo = f.openSong->package().canUndo();
    const auto undoLabel = f.openSong->package().undoLabel();

    run (f, SongIntent { SetExportFormat { ExportFormat::mp4 } });
    run (f, SongIntent { SetExportLength { VideoLength::fullSong } });
    setExportDestination (*f.window, folder.root.getChildFile ("Band Mix"));
    CHECK (f.song().settings.exportFormat == ExportFormat::mp4);
    CHECK (f.song().settings.exportLength == VideoLength::fullSong);
    CHECK (f.song().settings.exportDestination == folder.root.getChildFile ("Band Mix").getFullPathName().toStdString());
    CHECK (f.openSong->package().canUndo() == canUndo);
    CHECK (f.openSong->package().undoLabel() == undoLabel);

    reopenSong (f);
    auto& sheet = openExportSheet (*f.window);
    CHECK (sheet.mp4Button.getToggleState());
    CHECK (! sheet.wavButton.getToggleState());
    CHECK (sheet.request().videoLength == VideoLength::fullSong);
    CHECK (sheet.destination() == folder.root.getChildFile ("Band Mix.mp4"));
    CHECK (sheet.destinationLabel.getText() == folder.root.getChildFile ("Band Mix.mp4").getFullPathName());

    // The destination keeps its folder and name when the format changes; only its extension follows.
    click (sheet.m4aButton);
    CHECK (sheet.destination() == folder.root.getChildFile ("Band Mix.m4a"));
}

TEST_CASE ("a format chosen in the Export sheet is remembered when the sheet is closed with Esc", "[ui][export][choices]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    auto& sheet = openExportSheet (window);
    click (sheet.m4aButton);
    REQUIRE (pressKey (window, juce::KeyPress::escapeKey));
    REQUIRE (! window.songView()->isExportSheetShown());
    CHECK (f.song().settings.exportFormat == ExportFormat::m4a);

    reopenSong (f);
    CHECK (f.song().settings.exportFormat == ExportFormat::m4a);
    CHECK (openExportSheet (*f.window).m4aButton.getToggleState());
}

TEST_CASE ("when the destination file exists the sheet says it replaces it, and Export replaces it without asking", "[ui][export][choices][replace]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    int dialogs = 0;
    window.chooseExportDestination = [&dialogs] (juce::File, std::function<void (std::optional<juce::File>)>) { ++dialogs; };
    const auto file = folder.root.getChildFile ("Band Mix.wav");
    REQUIRE (file.replaceWithText ("an older export"));

    auto& sheet = openExportSheet (window);
    setExportDestination (window, file);
    REQUIRE (isShown (sheet.replacesLabel));
    CHECK (sheet.replacesLabel.getText() == juce::String ("Replaces the existing Band Mix.wav"));

    click (sheet.exportButton);
    tickUntilExported (f);
    CHECK (dialogs == 0);
    CHECK (f.alertsShown.empty());
    REQUIRE (f.openSong->lastExportedFile() == file);
    const auto decoded = overdub::song::decodeAudioFile (file, f.song().sampleRate);
    REQUIRE (decoded.has_value());
    CHECK (decoded->audio.getNumSamples() == overdub::exporting::lastClipEnd (f.song()));

    // Another file name: nothing is there to replace.
    setExportDestination (window, folder.root.getChildFile ("Other Mix.wav"));
    CHECK (! isShown (openExportSheet (window).replacesLabel));
}

TEST_CASE ("a saved destination whose folder is gone opens the sheet on <export folder>/<Song name>.<ext> with the saved format and options, and Export writes there", "[ui][export][choices][folder]")
{
    ExportFolder folder;
    MainWindowFixture f;
    f.window->exportFolder = folder.root.getChildFile ("Exports");
    const auto gone = folder.root.getChildFile ("Mixes");
    REQUIRE (gone.createDirectory().wasOk());
    run (f, SongIntent { SetExportFormat { ExportFormat::m4a } });
    run (f, SongIntent { SetExportLength { VideoLength::fullSong } });
    setExportDestination (*f.window, gone.getChildFile ("Band Mix.m4a"));
    REQUIRE (gone.deleteRecursively());

    reopenSong (f);
    auto& sheet = openExportSheet (*f.window);
    const auto fallback = folder.root.getChildFile ("Exports/" + songName (f) + ".m4a");
    CHECK (sheet.destination() == fallback);
    CHECK (sheet.destinationLabel.getText() == fallback.getFullPathName());
    CHECK (sheet.m4aButton.getToggleState());
    CHECK (sheet.request().videoLength == VideoLength::fullSong);
    // The saved destination is kept: its folder may come back (a drive plugged in again).
    CHECK (f.song().settings.exportDestination == gone.getChildFile ("Band Mix.m4a").getFullPathName().toStdString());

    click (sheet.exportButton);
    tickUntilExported (f);
    CHECK (f.openSong->lastExportedFile() == fallback);
    CHECK (fallback.existsAsFile());
    CHECK (! gone.exists());

    // Back again, the folder is used again.
    REQUIRE (gone.createDirectory().wasOk());
    CHECK (openExportSheet (*f.window).destination() == gone.getChildFile ("Band Mix.m4a"));
}

TEST_CASE ("a socket export whose destination folder is gone fails with export.folder_missing and makes no folder", "[ui][export][choices][folder][control]")
{
    ExportFolder folder;
    MainWindowFixture f;
    const auto gone = folder.root.getChildFile ("Mixes");
    REQUIRE (gone.createDirectory().wasOk());
    setExportDestination (*f.window, gone.getChildFile ("Band Mix.wav"));
    REQUIRE (gone.deleteRecursively());

    ScopedLogCapture capture;
    const auto exported = f.window->run (SongIntent { Export {} }, Origin::control);
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.folder_missing");
    CHECK (exported.error().fields.at ("path") == gone.getFullPathName().toStdString());
    CHECK (! f.openSong->isExporting());
    CHECK (! gone.exists());
    CHECK (eventsNamed (capture, "ui.export_requested").empty());
    CHECK (f.window->noticeLine.text() == juce::String ("That folder does not exist. Choose another place to export to."));
}

TEST_CASE ("set_export_destination into a folder that does not exist is export.folder_missing, and the destination stays", "[ui][export][choices][folder][control]")
{
    ExportFolder folder;
    MainWindowFixture f;
    const auto kept = f.song().settings.exportDestination;
    const auto gone = folder.root.getChildFile ("Mixes");

    const auto result = f.window->run (SongIntent { overdub::ui::intent::SetExportDestination { { gone.getChildFile ("Band Mix.wav").getFullPathName().toStdString() } } }, Origin::control);
    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "export.folder_missing");
    CHECK (f.song().settings.exportDestination == kept);
    CHECK (! gone.exists());
}

TEST_CASE ("a chosen file in the export folder does not make the export folder again", "[ui][export][choices][folder][control]")
{
    ExportFolder folder;
    MainWindowFixture f;
    f.window->exportFolder = folder.root.getChildFile ("Overdub");
    REQUIRE (f.window->exportFolder.createDirectory().wasOk());
    setExportDestination (*f.window, f.window->exportFolder.getChildFile ("Band Mix.wav"));
    REQUIRE (f.window->exportFolder.deleteRecursively());

    const auto exported = f.window->run (SongIntent { Export {} }, Origin::control);
    REQUIRE_FALSE (exported.has_value());
    CHECK (exported.error().code == "export.folder_missing");
    CHECK (! f.window->exportFolder.exists());
}

TEST_CASE ("the export folder is made on the first export into it", "[ui][export][choices][folder]")
{
    ExportFolder folder;
    MainWindowFixture f;
    f.window->exportFolder = folder.root.getChildFile ("Overdub");
    REQUIRE (! f.window->exportFolder.exists());

    run (f, SongIntent { Export {} });
    tickUntilExported (f);
    CHECK (f.openSong->lastExportedFile() == f.window->exportFolder.getChildFile (songName (f) + ".wav"));
    CHECK (f.window->exportFolder.getChildFile (songName (f) + ".wav").existsAsFile());
}
