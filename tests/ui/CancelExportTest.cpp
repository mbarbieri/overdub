#include "ui/TopBar.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

// Cancelling a running export (issue #202): the top bar's ✕ beside the export's progress runs
// cancel_export, which leaves the Song idle with a Notice and keeps the file that was at the
// destination; cancel_export is refused while no export runs.
using overdub::core::ScopedLogCapture;
using overdub::ui::ActivityKind;
using overdub::ui::Origin;
using overdub::ui::SongIntent;
using overdub::ui::intent::CancelExport;
using overdub::ui::intent::Export;
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
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-cancel-export-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE ("the top bar's ✕ cancels the running export: the Song is idle, a Notice says so and the file that was there stays", "[ui][export][cancel]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    const auto destination = folder.root.getChildFile ("Song.wav");
    REQUIRE (destination.replaceWithText ("before the export"));
    setExportDestination (window, destination);
    CHECK (! isShown (window.topBar.cancelExportButton));

    REQUIRE (window.run (SongIntent { Export {} }, Origin::control).has_value());
    REQUIRE (f.openSong->isExporting()); // the engine has not let go of the plugins: it waits
    REQUIRE (isShown (window.topBar.cancelExportButton));
    CHECK (window.topBar.cancelExportButton.isEnabled());
    ScopedLogCapture capture;

    click (window.topBar.cancelExportButton);

    CHECK (! f.openSong->isExporting());
    CHECK (! f.openSong->lastExportedFile().has_value());
    CHECK (destination.loadFileAsString() == "before the export");
    CHECK (folder.root.getNumberOfChildFiles (juce::File::findFiles, "*") == 1);
    CHECK (window.noticeLine.text() == juce::String ("The export was cancelled."));
    REQUIRE (window.noticeShown().has_value());
    CHECK (window.noticeShown()->message.code == "ui.export_cancelled");
    CHECK (! isShown (window.topBar.cancelExportButton));
    CHECK (isShown (window.topBar.exportButton));
    CHECK (window.topBar.exportButton.isEnabled());
    const auto cancelled = eventsNamed (capture, "ui.export_cancelled");
    REQUIRE (cancelled.size() == 1);
    CHECK (cancelled[0]["path"] == destination.getFullPathName().toStdString());
    const auto ran = eventsNamed (capture, "ui.intent_run");
    REQUIRE (ran.size() == 1);
    CHECK (ran[0]["op"] == "cancel_export");
    const auto& finish = f.openSong->activities().lastFinish();
    REQUIRE (finish.has_value());
    CHECK (finish->kind == ActivityKind::exportSong);
    CHECK (finish->errorCode == "ui.export_cancelled");

    // The engine plays the Song again.
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.engine.isPlaying());
}

TEST_CASE ("cancel_export while no export runs is refused with ui.no_export_running", "[ui][export][cancel][control]")
{
    MainWindowFixture f;
    auto& window = *f.window;

    const auto refused = window.run (SongIntent { CancelExport {} }, Origin::control);

    REQUIRE (! refused.has_value());
    CHECK (refused.error().code == "ui.no_export_running");
    CHECK (! window.noticeShown().has_value());
}
