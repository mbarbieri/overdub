#include "ui/ExportSheet.h"

#include "export/MixRender.h"
#include "song/AudioFileDecode.h"
#include "ui/ExportSheetSink.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// The Export sheet (issues #37, #200, ADR 0005 UI amendment): Export... opens it over the window,
// the format choice and destination flow into the export request, Export closes it while the top
// bar shows the export's progress, and the finished export's Notice reveals the written file.
using overdub::core::ScopedLogCapture;
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportRequest;
using overdub::ui::ExportPhase;
using overdub::ui::ExportSheet;
using overdub::ui::Origin;
using overdub::ui::SongIntent;
using overdub::ui::intent::Export;
using overdub::ui::intent::ShowExport;
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
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-export-ui-" + juce::Uuid().toString());

    ExportFolder() { root.createDirectory(); }
    ~ExportFolder() { root.deleteRecursively(); }
};

// Polls the window, as its timer would, until the export has ended (or 20 s have passed);
// returns the progress the top bar showed at each poll while the export ran.
std::vector<double> tickUntilExported (MainWindowFixture& f)
{
    std::vector<double> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        if (isShown (f.window->topBar.exportProgressBar))
            seen.push_back (f.window->topBar.exportProgressBar.fraction());
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return seen;
}

bool sheetShown (MainWindowFixture& f)
{
    return f.window->songView()->isExportSheetShown() && isShown (f.window->songView()->exportSheet);
}
} // namespace

TEST_CASE ("the Export sheet's span and the top bar's export status read as the prototype's", "[ui][export]")
{
    using overdub::ui::exportSpanText;
    using overdub::ui::exportStatusText;
    CHECK (exportSpanText (0, 48000) == "Nothing to export: the Song has no Clips");
    CHECK (exportSpanText (48000 * 83 + 24000, 48000) == "Whole mix: 0:00.0 \xe2\x80\x93 1:23.5, plus effect tails");
    CHECK (exportStatusText (true, 0.0) == "Preparing\xe2\x80\xa6");
    CHECK (exportStatusText (false, 0.429) == "Exporting\xe2\x80\xa6 42%");
    CHECK (exportStatusText (false, 1.0) == "Exporting\xe2\x80\xa6 100%");
}

TEST_CASE ("the format choice and the destination flow into the export request", "[ui][export]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportSheet sheet;
    layOut (sheet, windowWidth, 600);
    sheet.setDestination (juce::File ("/Users/someone/Music/Overdub/My Song.wav"));
    ExportSheetSink sink;
    sink.connect (sheet);
    const auto& requests = sink.requests;
    std::vector<juce::File> askedFrom;
    sheet.onChooseDestination = [&askedFrom] (juce::File current) { askedFrom.push_back (current); };

    CHECK (sheet.wavButton.getToggleState());
    CHECK (sheet.mp4Button.isEnabled()); // issue #38
    click (sheet.exportButton);
    REQUIRE (requests.size() == 1);
    CHECK (requests[0] == ExportRequest { ExportFormat::wav, juce::File ("/Users/someone/Music/Overdub/My Song.wav") });

    click (sheet.m4aButton);
    CHECK (sheet.m4aButton.getToggleState());
    CHECK (! sheet.wavButton.getToggleState());
    CHECK (sheet.destinationLabel.getText() == "/Users/someone/Music/Overdub/My Song.m4a");

    click (sheet.chooseButton);
    REQUIRE (askedFrom.size() == 1);
    CHECK (askedFrom[0] == juce::File ("/Users/someone/Music/Overdub/My Song.m4a"));
    sheet.setDestination (juce::File ("/Volumes/Share/Mixes/take two")); // what the dialog chose

    click (sheet.exportButton);
    REQUIRE (requests.size() == 2);
    CHECK (requests[1] == ExportRequest { ExportFormat::m4a, juce::File ("/Volumes/Share/Mixes/take two.m4a") });

    click (sheet.cancelButton);
    CHECK (sink.closes == 1);
}

TEST_CASE ("a format toggle the handler does not call back leaves the form as it was", "[ui][export]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportSheet sheet;
    layOut (sheet, windowWidth, 600);
    sheet.setDestination (juce::File ("/Users/someone/Music/Overdub/My Song.wav"));
    std::vector<overdub::ui::Intent> refused;
    sheet.onIntent = [&refused] (const overdub::ui::Intent& intent) { refused.push_back (intent); };

    click (sheet.mp4Button);
    CHECK (refused.size() == 1);
    CHECK (sheet.wavButton.getToggleState());
    CHECK_FALSE (sheet.mp4Button.getToggleState());
    CHECK (sheet.request() == ExportRequest { ExportFormat::wav, juce::File ("/Users/someone/Music/Overdub/My Song.wav") });
}

TEST_CASE ("Export is no mode: the top bar offers Record, Edit and Export..., and key 3 does nothing", "[ui][export][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    CHECK (isShown (window.topBar.recordModeButton));
    CHECK (isShown (window.topBar.editModeButton));
    REQUIRE (isShown (window.topBar.exportButton));
    CHECK (window.topBar.exportButton.getButtonText() == juce::String::fromUTF8 ("Export\xe2\x80\xa6"));
    CHECK (window.topBar.exportButton.isEnabled());
    CHECK (! sheetShown (f));

    CHECK (! pressKey (window, '3'));
    CHECK (window.mode() == overdub::ui::Mode::record);
    CHECK (! sheetShown (f));
}

TEST_CASE ("with no Song open Export... is unavailable and show_export answers that no Song is open", "[ui][export][main]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    overdub::ui::MainComponent window;
    layOut (window, windowWidth, windowHeight);

    CHECK (isShown (window.topBar.exportButton));
    CHECK (! window.topBar.exportButton.isEnabled());
    const auto result = window.run (SongIntent { ShowExport { true } }, Origin::control);
    REQUIRE (! result.has_value());
    CHECK (result.error().code == "ui.no_song_open");
    CHECK (pressKey (window, 'e', juce::ModifierKeys::commandModifier));
}

TEST_CASE ("Export... in either mode, and command-E, open the Export sheet over the window; Cancel and Esc close it", "[ui][export][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    const auto modeKey = GENERATE ('1', '2');
    REQUIRE (pressKey (window, (juce::juce_wchar) modeKey));

    ScopedLogCapture capture;
    click (window.topBar.exportButton);
    REQUIRE (sheetShown (f));
    // Over the whole window, the top bar included.
    CHECK (window.songView()->exportSheet.getBounds() == window.getLocalBounds());
    const auto shown = eventsNamed (capture, "ui.export_sheet_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0]["shown"] == true);

    click (window.songView()->exportSheet.cancelButton);
    CHECK (! sheetShown (f));

    REQUIRE (pressKey (window, 'e', juce::ModifierKeys::commandModifier));
    REQUIRE (sheetShown (f));
    REQUIRE (pressKey (window, juce::KeyPress::escapeKey));
    CHECK (! sheetShown (f));
    CHECK (window.mode() == (modeKey == '1' ? overdub::ui::Mode::record : overdub::ui::Mode::edit));
}

TEST_CASE ("the Export sheet is modal: keys other than Esc and Return do nothing while it shows", "[ui][export][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    openExportSheet (window);

    CHECK (pressKey (window, '2'));
    CHECK (window.mode() == overdub::ui::Mode::record);
    CHECK (pressKey (window, 'r'));
    f.runBlocks (2);
    CHECK (! f.openSong->isRecording());
    CHECK (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (! f.engine.isPlaying());
    CHECK (sheetShown (f));
}

TEST_CASE ("Return in the Export sheet exports what it shows and closes it", "[ui][export][main]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    auto& sheet = openExportSheet (window);
    sheet.setDestination (folder.root.getChildFile ("Returned.wav"));

    REQUIRE (pressKey (window, juce::KeyPress::returnKey));
    CHECK (! sheetShown (f));
    CHECK (f.openSong->isExporting());
    tickUntilExported (f);
    CHECK (f.openSong->lastExportedFile() == folder.root.getChildFile ("Returned.wav"));
}

TEST_CASE ("Return does nothing while the Export sheet's Export is disabled", "[ui][export][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    auto& sheet = openExportSheet (window);
    click (sheet.mp4Button); // the fixture Song has no Video Take to show
    REQUIRE (! sheet.exportButton.isEnabled());

    ScopedLogCapture capture;
    CHECK (pressKey (window, juce::KeyPress::returnKey));
    CHECK (sheetShown (f));
    CHECK (! f.openSong->isExporting());
    CHECK (eventsNamed (capture, "ui.export_requested").empty());
    CHECK (eventsNamed (capture, "ui.notice_shown").empty());
}

TEST_CASE ("Export... is unavailable during a Pass and while an export runs, and show_export is refused then", "[ui][export][availability]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;

    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (20);
    REQUIRE (f.openSong->isRecording());
    CHECK (! window.topBar.exportButton.isEnabled());
    const auto duringPass = window.run (SongIntent { ShowExport { true } }, Origin::control);
    REQUIRE (! duringPass.has_value());
    CHECK (duringPass.error().code == "ui.stop_recording_first");
    CHECK (! sheetShown (f));
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (2);
    REQUIRE (! f.openSong->isRecording());
    CHECK (window.topBar.exportButton.isEnabled());

    REQUIRE (f.openSong->startExport ({ ExportFormat::wav, folder.root.getChildFile ("Song.wav") }).has_value());
    window.tick();
    CHECK (! isShown (window.topBar.exportButton));
    const auto duringExport = window.run (SongIntent { ShowExport { true } }, Origin::control);
    REQUIRE (! duringExport.has_value());
    CHECK (duringExport.error().code == "ui.export_running");
    CHECK (! sheetShown (f));
    tickUntilExported (f);
}

TEST_CASE ("the Export sheet exports the open Song's mix to the chosen file, the top bar shows the progress, and the Notice reveals the file", "[ui][export]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    std::vector<juce::File> revealed;
    window.revealInFinder = [&revealed] (const juce::File& file) { revealed.push_back (file); };
    const auto chosen = folder.root.getChildFile ("Band Mix export");
    window.chooseExportDestination = [chosen] (juce::File, std::function<void (std::optional<juce::File>)> done) { done (chosen); };

    auto& sheet = openExportSheet (window);
    CHECK (sheet.destination().getFileName().endsWith (".wav"));
    click (sheet.m4aButton);
    click (sheet.chooseButton);
    CHECK (sheet.destination() == folder.root.getChildFile ("Band Mix export.m4a"));

    ScopedLogCapture capture;
    f.engine.play();
    f.runBlocks (2);
    REQUIRE (f.engine.isPlaying());
    click (sheet.exportButton);

    // The sheet closes (logged as show_export logs it); the engine stops and must hand back the plugins before the export renders them.
    CHECK (! sheetShown (f));
    CHECK (f.openSong->exportPhase() == ExportPhase::waitingForAudio);
    CHECK (! isShown (window.topBar.exportButton));
    REQUIRE (isShown (window.topBar.exportStatusLabel));
    CHECK (isShown (window.topBar.exportProgressBar));
    CHECK (window.topBar.exportStatusLabel.getText() == juce::String::fromUTF8 ("Preparing\xe2\x80\xa6"));
    window.tick();
    CHECK (f.openSong->exportPhase() == ExportPhase::waitingForAudio);

    // R and Space do nothing while it runs; R says why.
    REQUIRE (pressKey (window, 'r'));
    CHECK (! f.openSong->isRecording());
    CHECK (window.noticeLine.text() == juce::String ("Wait for the export to finish."));
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (! f.engine.isPlaying());

    const auto seen = tickUntilExported (f);
    REQUIRE (! f.openSong->isExporting());
    REQUIRE (! seen.empty());
    CHECK (std::ranges::is_sorted (seen));

    const auto file = folder.root.getChildFile ("Band Mix export.m4a");
    REQUIRE (f.openSong->lastExportedFile() == file);
    REQUIRE (file.existsAsFile());
    const auto decoded = overdub::song::decodeAudioFile (file, f.song().sampleRate);
    REQUIRE (decoded.has_value());
    CHECK (decoded->audio.getNumSamples() == overdub::exporting::lastClipEnd (f.song()));

    // The top bar offers Export... again, and the Notice names the file and reveals it.
    CHECK (isShown (window.topBar.exportButton));
    CHECK (window.topBar.exportButton.isEnabled());
    CHECK (! isShown (window.topBar.exportStatusLabel));
    CHECK (! isShown (window.topBar.exportProgressBar));
    CHECK (window.noticeLine.text() == juce::String ("Exported to Band Mix export.m4a."));
    REQUIRE (window.noticeLine.hasAction());
    CHECK (window.noticeLine.actionButton.getButtonText() == juce::String ("Reveal in Finder"));
    REQUIRE (window.noticeShown().has_value());
    CHECK (window.noticeShown()->message.code == "ui.export_finished");
    window.tick();
    CHECK (isShown (window.noticeLine)); // a Notice with an action stays until it is clicked or replaced
    click (window.noticeLine.actionButton);
    CHECK (revealed == std::vector<juce::File> { file });

    const auto closed = eventsNamed (capture, "ui.export_sheet_shown");
    REQUIRE (closed.size() == 1);
    CHECK (closed[0]["shown"] == false);
    const auto requested = eventsNamed (capture, "ui.export_requested");
    REQUIRE (requested.size() == 1);
    CHECK (requested[0]["format"] == "m4a");
    CHECK (requested[0]["path"] == file.getFullPathName().toStdString());
    CHECK (eventsNamed (capture, "export.finished").size() == 1);
    const auto noticed = eventsNamed (capture, "ui.notice_shown");
    CHECK (std::ranges::count_if (noticed, [] (const nlohmann::json& event) { return event["code"] == "ui.export_finished"; }) == 1);

    // The engine plays the Song again.
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.engine.isPlaying());
}

TEST_CASE ("an export run over the socket with no sheet, in Edit mode, shows the same progress and Notice", "[ui][export][control]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    REQUIRE (pressKey (window, '2'));
    REQUIRE (window.run (SongIntent { overdub::ui::intent::SetExportDestination { { folder.root.getChildFile ("Socket.wav").getFullPathName().toStdString() } } }, Origin::control).has_value());

    REQUIRE (window.run (SongIntent { Export {} }, Origin::control).has_value());
    CHECK (! sheetShown (f));
    CHECK (isShown (window.topBar.exportStatusLabel));
    CHECK (isShown (window.songView()->editPage));
    tickUntilExported (f);

    CHECK (f.openSong->lastExportedFile() == folder.root.getChildFile ("Socket.wav"));
    CHECK (window.noticeLine.text() == juce::String ("Exported to Socket.wav."));
    CHECK (window.noticeLine.hasAction());
}

TEST_CASE ("an export that cannot write its file is an Alert, and Export... is offered again", "[ui][export]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    // A regular file stands where the destination's folder should be.
    ExportFolder folder;
    const auto blocker = folder.root.getChildFile ("not-a-folder");
    REQUIRE (blocker.replaceWithText ("x"));
    auto& sheet = openExportSheet (window);
    sheet.setDestination (blocker.getChildFile ("Song.wav"));

    ScopedLogCapture capture;
    click (sheet.exportButton);
    tickUntilExported (f);
    REQUIRE (! f.openSong->isExporting());

    CHECK (f.alertsShown == std::vector<juce::String> { "The export could not be written there." });
    const auto failed = eventsNamed (capture, "export.write_failed");
    REQUIRE (failed.size() == 1);
    CHECK (failed[0]["lvl"] == "error");
    CHECK (failed[0]["step"] == "folder");
    CHECK (! f.openSong->lastExportedFile().has_value());
    CHECK (eventsNamed (capture, "ui.notice_shown").empty());
    CHECK (isShown (window.topBar.exportButton));
    CHECK (window.topBar.exportButton.isEnabled());
}

TEST_CASE ("a Song without Clips is a Notice, not an export", "[ui][export]")
{
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}.track ("Guitar", true, { 0 }).build() };
    ExportFolder folder;
    auto& window = *f.window;
    auto& sheet = openExportSheet (window);
    sheet.setDestination (folder.root.getChildFile ("Empty.wav"));

    ScopedLogCapture capture;
    click (sheet.exportButton);
    tickUntilExported (f);

    CHECK (window.noticeLine.text() == juce::String ("The Song has no Clips to export."));
    CHECK (! window.noticeLine.hasAction());
    const auto refused = eventsNamed (capture, "export.nothing_to_export");
    REQUIRE (refused.size() == 1);
    CHECK (refused[0]["lvl"] == "warn");
    CHECK (! folder.root.getChildFile ("Empty.wav").exists());
}

TEST_CASE ("closing the audio stops a running export", "[ui][export]")
{
    MainWindowFixture f;
    ExportFolder folder;
    ScopedLogCapture capture;
    REQUIRE (f.openSong->startExport ({ ExportFormat::wav, folder.root.getChildFile ("Song.wav") }).has_value());
    REQUIRE (f.openSong->exportPhase() == ExportPhase::waitingForAudio);

    CHECK (f.openSong->detachAudio().empty());
    CHECK (! f.openSong->isExporting());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "audio_detached");
    CHECK (stopped[0]["phase"] == "waiting_for_audio");
}

TEST_CASE ("an export whose audio device stopped calling back fails with an Alert after a bounded wait", "[ui][export]")
{
    MainWindowFixture f;
    ExportFolder folder;
    auto& window = *f.window;
    f.engine.play();
    f.runBlocks (2);
    REQUIRE (f.engine.isPlaying());

    // From here on the device never calls back: the audio thread never takes the empty plan.
    ScopedLogCapture capture;
    const auto destination = folder.root.getChildFile ("Song.wav");
    const auto requestedAt = std::chrono::steady_clock::now();
    REQUIRE (f.openSong->startExport ({ ExportFormat::wav, destination }, requestedAt).has_value());
    window.tick (requestedAt + overdub::ui::ExportRun::audioTimeout - std::chrono::milliseconds (1));
    CHECK (f.openSong->exportPhase() == ExportPhase::waitingForAudio);
    CHECK (f.alertsShown.empty());

    window.tick (requestedAt + overdub::ui::ExportRun::audioTimeout);
    CHECK (! f.openSong->isExporting());
    CHECK (f.alertsShown == std::vector<juce::String> { "The audio device stopped responding, so nothing was exported. Check the device, then export again." });
    const auto failed = eventsNamed (capture, "ui.export_audio_unresponsive");
    REQUIRE (failed.size() == 1);
    CHECK (failed[0]["lvl"] == "error");
    CHECK (failed[0]["path"] == destination.getFullPathName().toStdString());
    CHECK (failed[0]["waited_ms"] == std::chrono::milliseconds (overdub::ui::ExportRun::audioTimeout).count());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "audio_unresponsive");
    CHECK (stopped[0]["phase"] == "waiting_for_audio");
    CHECK (! destination.exists());
    CHECK (isShown (window.topBar.exportButton));
    CHECK (! isShown (window.topBar.exportStatusLabel));

    // Once the device calls back again it plays the Song, and the next export runs.
    f.engine.play();
    f.runBlocks (1);
    CHECK (f.engine.isPlaying());
    REQUIRE (f.openSong->startExport ({ ExportFormat::wav, destination }).has_value());
    tickUntilExported (f);
    CHECK (f.openSong->lastExportedFile() == destination);
    CHECK (destination.existsAsFile());
}

TEST_CASE ("snapshots of the Export sheet with WAV selected, the top bar during an export, and after it", "[ui][snapshot]")
{
    const std::string slice = "slice-16";
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    window.exportFolder = folder.root;
    window.setOpenSong (f.openSong.get()); // the destination follows the export folder
    auto& sheet = openExportSheet (window);
    REQUIRE (sheet.wavButton.getToggleState());
    CHECK (writeSnapshot (window, slice, "export-wav").existsAsFile());

    click (sheet.exportButton);
    CHECK (writeSnapshot (window, slice, "export-running").existsAsFile());
    tickUntilExported (f);
    REQUIRE (f.openSong->lastExportedFile().has_value());
    CHECK (writeSnapshot (window, slice, "export-finished").existsAsFile());
}
