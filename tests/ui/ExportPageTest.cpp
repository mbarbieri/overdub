#include "ui/ExportPage.h"

#include "export/MixRender.h"
#include "song/AudioFileDecode.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// Export mode's page (issue #37, ADR 0005 UI amendment): the format choice and destination flow
// into the export request, the Export button is disabled while an export runs, progress
// advances, and Reveal in Finder shows the written file.
using overdub::core::ScopedLogCapture;
using overdub::exporting::ExportFormat;
using overdub::exporting::ExportRequest;
using overdub::ui::ExportPage;
using overdub::ui::ExportPageState;
using overdub::ui::ExportPhase;
using namespace overdub::ui::testing;

namespace
{
bool near (double a, double b)
{
    return std::abs (a - b) < 1.0e-9;
}

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
// returns the progress the page showed at each poll.
std::vector<double> tickUntilExported (MainWindowFixture& f)
{
    std::vector<double> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
    while (f.openSong->isExporting() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        seen.push_back (f.window->exportPage.progress());
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    return seen;
}
} // namespace

TEST_CASE ("the Export page's span and status read as the prototype's", "[ui][export]")
{
    using overdub::ui::exportSpanText;
    using overdub::ui::exportStatusText;
    CHECK (exportSpanText (0, 48000) == "Whole mix: 0:00.0 \xe2\x80\x93 0:00.0, plus effect tails");
    CHECK (exportSpanText (48000 * 83 + 24000, 48000) == "Whole mix: 0:00.0 \xe2\x80\x93 1:23.5, plus effect tails");
    CHECK (exportStatusText ({}) == "");
    CHECK (exportStatusText ({ .waitingForAudio = true }) == "Preparing\xe2\x80\xa6");
    CHECK (exportStatusText ({ .rendering = true, .progress = 0.429 }) == "Exporting\xe2\x80\xa6 42%");
    CHECK (exportStatusText ({ .finishedFile = juce::File ("/tmp/Song.wav") }) == "Exported to /tmp/Song.wav");
}

TEST_CASE ("the format choice and the destination flow into the export request", "[ui][export]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportPage page;
    layOut (page, windowWidth, 600);
    page.setDestination (juce::File ("/Users/someone/Music/Overdub/My Song.wav"));
    std::vector<ExportRequest> requests;
    page.onExport = [&requests] (ExportRequest request) { requests.push_back (request); };
    std::vector<juce::File> askedFrom;
    page.onChooseDestination = [&askedFrom] (juce::File current) { askedFrom.push_back (current); };

    CHECK (page.wavButton.getToggleState());
    CHECK (page.mp4Button.isEnabled()); // issue #38
    click (page.exportButton);
    REQUIRE (requests.size() == 1);
    CHECK (requests[0] == ExportRequest { ExportFormat::wav, juce::File ("/Users/someone/Music/Overdub/My Song.wav") });

    click (page.m4aButton);
    CHECK (page.m4aButton.getToggleState());
    CHECK (! page.wavButton.getToggleState());
    CHECK (page.destinationLabel.getText() == "/Users/someone/Music/Overdub/My Song.m4a");

    click (page.chooseButton);
    REQUIRE (askedFrom.size() == 1);
    CHECK (askedFrom[0] == juce::File ("/Users/someone/Music/Overdub/My Song.m4a"));
    page.setDestination (juce::File ("/Volumes/Share/Mixes/take two")); // what the dialog chose

    click (page.exportButton);
    REQUIRE (requests.size() == 2);
    CHECK (requests[1] == ExportRequest { ExportFormat::m4a, juce::File ("/Volumes/Share/Mixes/take two.m4a") });
}

TEST_CASE ("the Export button is disabled while an export runs, and progress advances", "[ui][export]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ExportPage page;
    layOut (page, windowWidth, 600);
    page.setDestination (juce::File ("/tmp/Song.wav"));
    CHECK (page.exportButton.isEnabled());
    CHECK (! isShown (page.revealButton));

    page.show ({ .clipsEnd = 48000, .waitingForAudio = true });
    CHECK (! page.exportButton.isEnabled());
    CHECK (! page.wavButton.isEnabled());
    CHECK (! page.chooseButton.isEnabled());
    CHECK (near (page.progress(), 0.0));

    page.show ({ .clipsEnd = 48000, .rendering = true, .progress = 0.25 });
    CHECK (! page.exportButton.isEnabled());
    CHECK (near (page.progress(), 0.25));
    page.show ({ .clipsEnd = 48000, .rendering = true, .progress = 0.75 });
    CHECK (near (page.progress(), 0.75));
    CHECK (page.statusLabel.getText() == juce::String::fromUTF8 ("Exporting\xe2\x80\xa6 75%"));

    std::vector<juce::File> revealed;
    page.onReveal = [&revealed] (juce::File file) { revealed.push_back (file); };
    page.show ({ .clipsEnd = 48000, .finishedFile = juce::File ("/tmp/Song.wav") });
    CHECK (page.exportButton.isEnabled());
    CHECK (near (page.progress(), 1.0));
    REQUIRE (isShown (page.revealButton));
    click (page.revealButton);
    CHECK (revealed == std::vector<juce::File> { juce::File ("/tmp/Song.wav") });
}

TEST_CASE ("Export mode exports the open Song's mix to the chosen file and reveals it", "[ui][export]")
{
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    std::vector<juce::File> revealed;
    window.revealInFinder = [&revealed] (const juce::File& file) { revealed.push_back (file); };
    const auto chosen = folder.root.getChildFile ("Band Mix export");
    window.chooseExportDestination = [chosen] (juce::File, std::function<void (std::optional<juce::File>)> done) { done (chosen); };

    REQUIRE (pressKey (window, '3'));
    auto& page = window.exportPage;
    CHECK (page.destination().getFileName().endsWith (".wav"));
    click (page.m4aButton);
    click (page.chooseButton);
    CHECK (page.destination() == folder.root.getChildFile ("Band Mix export.m4a"));

    ScopedLogCapture capture;
    f.engine.play();
    f.runBlocks (2);
    REQUIRE (f.engine.isPlaying());
    click (page.exportButton);

    // The engine stops and must hand back the plugins before the export renders them.
    CHECK (f.openSong->exportPhase() == ExportPhase::waitingForAudio);
    CHECK (! page.exportButton.isEnabled());
    CHECK (page.statusLabel.getText() == juce::String::fromUTF8 ("Preparing\xe2\x80\xa6"));
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
    CHECK (near (seen.back(), 1.0));

    const auto file = folder.root.getChildFile ("Band Mix export.m4a");
    REQUIRE (f.openSong->lastExportedFile() == file);
    REQUIRE (file.existsAsFile());
    const auto decoded = overdub::song::decodeAudioFile (file, f.song().sampleRate);
    REQUIRE (decoded.has_value());
    CHECK (decoded->audio.getNumSamples() == overdub::exporting::lastClipEnd (f.song()));

    CHECK (page.exportButton.isEnabled());
    REQUIRE (isShown (page.revealButton));
    click (page.revealButton);
    CHECK (revealed == std::vector<juce::File> { file });

    const auto requested = eventsNamed (capture, "ui.export_requested");
    REQUIRE (requested.size() == 1);
    CHECK (requested[0]["format"] == "m4a");
    CHECK (requested[0]["path"] == file.getFullPathName().toStdString());
    CHECK (eventsNamed (capture, "export.finished").size() == 1);

    // The engine plays the Song again.
    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.engine.isPlaying());
}

TEST_CASE ("an export that cannot write its file is an Alert, and the page is ready again", "[ui][export]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    REQUIRE (pressKey (window, '3'));
    // A regular file stands where the destination's folder should be.
    ExportFolder folder;
    const auto blocker = folder.root.getChildFile ("not-a-folder");
    REQUIRE (blocker.replaceWithText ("x"));
    window.exportPage.setDestination (blocker.getChildFile ("Song.wav"));

    ScopedLogCapture capture;
    click (window.exportPage.exportButton);
    tickUntilExported (f);
    REQUIRE (! f.openSong->isExporting());

    CHECK (f.alertsShown == std::vector<juce::String> { "The export could not be written there." });
    const auto failed = eventsNamed (capture, "export.write_failed");
    REQUIRE (failed.size() == 1);
    CHECK (failed[0]["lvl"] == "error");
    CHECK (failed[0]["step"] == "folder");
    CHECK (! f.openSong->lastExportedFile().has_value());
    CHECK (window.exportPage.exportButton.isEnabled());
    CHECK (! isShown (window.exportPage.revealButton));
}

TEST_CASE ("a Song without Clips is a Notice, not an export", "[ui][export]")
{
    MainWindowFixture f { overdub::song::testing::FixtureSongBuilder {}.track ("Guitar", true, { 0 }).build() };
    ExportFolder folder;
    auto& window = *f.window;
    REQUIRE (pressKey (window, '3'));
    window.exportPage.setDestination (folder.root.getChildFile ("Empty.wav"));

    ScopedLogCapture capture;
    click (window.exportPage.exportButton);
    tickUntilExported (f);

    CHECK (window.noticeLine.text() == juce::String ("The Song has no Clips to export."));
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

TEST_CASE ("snapshots of the Export page with WAV selected, and after an export", "[ui][snapshot]")
{
    const std::string slice = "slice-16";
    ExportFolder folder;
    MainWindowFixture f;
    auto& window = *f.window;
    window.exportFolder = folder.root;
    window.setOpenSong (f.openSong.get()); // the destination follows the export folder
    REQUIRE (pressKey (window, '3'));
    REQUIRE (window.exportPage.wavButton.getToggleState());
    CHECK (writeSnapshot (window, slice, "export-wav").existsAsFile());

    click (window.exportPage.exportButton);
    tickUntilExported (f);
    REQUIRE (f.openSong->lastExportedFile().has_value());
    CHECK (writeSnapshot (window, slice, "export-finished").existsAsFile());
}
