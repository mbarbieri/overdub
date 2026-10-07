#include "ui/MainComponent.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::ui::Mode;
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

std::string text (const juce::Button& button)
{
    return button.getButtonText().toStdString();
}
} // namespace

TEST_CASE ("keys 1 to 3 show the Record, Edit and Export pages", "[ui][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    CHECK (isShown (window.recordPage));

    CHECK (pressKey (window, '2'));
    CHECK (window.mode() == Mode::edit);
    CHECK (isShown (window.editPage));
    CHECK (! isShown (window.recordPage));
    CHECK (window.topBar.editModeButton.getToggleState());

    CHECK (pressKey (window, '3'));
    CHECK (isShown (window.exportPage));
    CHECK (! isShown (window.editPage));

    CHECK (pressKey (window, '1'));
    CHECK (isShown (window.recordPage));
    CHECK (! isShown (window.exportPage));
    CHECK (window.topBar.recordModeButton.getToggleState());

    CHECK (! pressKey (window, 'x'));
}

TEST_CASE ("clicking a mode button shows its page", "[ui][main]")
{
    MainWindowFixture f;
    click (f.window->topBar.exportModeButton);
    CHECK (isShown (f.window->exportPage));
    click (f.window->topBar.recordModeButton);
    CHECK (isShown (f.window->recordPage));
}

TEST_CASE ("R in any mode records a Pass and Space stops it, as one labelled undo step", "[ui][main][record]")
{
    const auto modeKey = GENERATE ('1', '2', '3');
    MainWindowFixture f;
    auto& window = *f.window;
    REQUIRE (pressKey (window, (juce::juce_wchar) modeKey));

    REQUIRE (pressKey (window, 'r'));
    f.runBlocks (20);
    CHECK (f.engine.isRecording());
    CHECK (window.topBar.clock.statusText() == juce::String (juce::CharPointer_UTF8 ("REC \xc2\xb7 PASS 1")));
    CHECK (! window.topBar.undoButton.isEnabled());

    REQUIRE (pressKey (window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    CHECK (! f.session->isRecording());
    CHECK (f.song().passes.size() == 1);
    CHECK (f.session->package().undoLabel() == "Record Pass 1");
    CHECK (text (window.topBar.undoButton) == "Undo Record Pass 1");
    CHECK (window.topBar.clock.statusText() == "STOP");
    CHECK (f.trackNamed ("Guitar").clips.size() == 1);
    CHECK (f.trackNamed ("Vocals").clips.size() == 1);
}

TEST_CASE ("the record and stop buttons record a Pass like R and Space", "[ui][main][record]")
{
    MainWindowFixture f;
    click (f.window->topBar.recordButton);
    f.runBlocks (10);
    CHECK (f.session->isRecording());
    click (f.window->topBar.playStopButton);
    f.runBlocks (1);
    CHECK (f.session->package().undoLabel() == "Record Pass 1");
}

TEST_CASE ("Undo and Redo buttons show the step labels and step through them", "[ui][main][history]")
{
    MainWindowFixture f;
    auto& topBar = f.window->topBar;
    CHECK (text (topBar.undoButton) == "Undo Add Tracks");
    CHECK (text (topBar.redoButton) == "Redo");
    CHECK (! topBar.redoButton.isEnabled());

    click (topBar.undoButton);
    CHECK (text (topBar.undoButton) == "Undo Import Band Mix.wav");
    CHECK (text (topBar.redoButton) == "Redo Add Tracks");
    CHECK (f.window->recordPage.rows.size() == 1);

    click (topBar.redoButton);
    CHECK (text (topBar.undoButton) == "Undo Add Tracks");
    CHECK (f.window->recordPage.rows.size() == 3);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (text (topBar.redoButton) == "Redo Add Tracks");
    REQUIRE (pressKey (*f.window, 'Z', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)));
    CHECK (text (topBar.undoButton) == "Undo Add Tracks");
}

TEST_CASE ("arm and the input picker change the Song without a history step", "[ui][main][rows]")
{
    MainWindowFixture f;
    const auto stepBefore = f.session->package().undoLabel();
    auto& guitar = f.rowNamed ("Guitar");
    CHECK (guitar.nameLabel.getText() == "Guitar");
    CHECK (guitar.armButton.getToggleState());
    CHECK (guitar.inputPicker.getText() == "In 1");

    click (guitar.armButton);
    CHECK (! f.trackNamed ("Guitar").armed);
    CHECK (! f.rowNamed ("Guitar").armButton.getToggleState());

    // No input, In 1..In 4, then the pairs: index 6 is "In 3 + In 4".
    choose (f.rowNamed ("Guitar").inputPicker, 6);
    CHECK (f.trackNamed ("Guitar").inputChannels == std::vector<int> { 2, 3 });
    CHECK (f.rowNamed ("Guitar").inputPicker.getText() == "In 3 + In 4");

    click (f.rowNamed ("Guitar").monitoringButton);
    CHECK (f.session->isMonitoring (f.trackNamed ("Guitar").id));
    CHECK (f.rowNamed ("Guitar").monitoringButton.getToggleState());

    CHECK (f.session->package().undoLabel() == stepBefore);
    CHECK (f.session->package().hasPendingWrite());
}

TEST_CASE ("an injected import adds a Track row named after the file", "[ui][main][import]")
{
    MainWindowFixture f;
    const auto file = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    f.window->chooseFilesToImport = [file] (std::function<void (std::vector<juce::File>)> done) { done ({ file }); };

    REQUIRE (f.window->recordPage.rows.size() == 3);
    click (f.window->recordPage.importButton);

    REQUIRE (f.window->recordPage.rows.size() == 4);
    CHECK (f.window->recordPage.rows.back()->nameLabel.getText() == "Drums");
    CHECK (text (f.window->topBar.undoButton) == "Undo Import Drums.wav");
}

TEST_CASE ("a failed import is an Alert", "[ui][main][import][messages]")
{
    MainWindowFixture f;
    const auto notAudio = f.fixture->folder.getChildFile ("notes.txt");
    REQUIRE (notAudio.replaceWithText ("not audio"));
    f.window->chooseFilesToImport = [notAudio] (std::function<void (std::vector<juce::File>)> done) { done ({ notAudio }); };

    ScopedLogCapture capture;
    click (f.window->recordPage.importButton);

    const auto shown = eventsNamed (capture, "ui.alert_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0].at ("code") == "song.import_unreadable");
    CHECK (shown[0].at ("tier") == "alert");
    CHECK (f.alertsShown.size() == 1);
    CHECK (f.window->recordPage.rows.size() == 3);
}

TEST_CASE ("an Alert raised during a Pass waits until the Pass stops; a Notice shows at once", "[ui][main][messages]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (4);

    ScopedLogCapture capture;
    f.window->handleError (overdub::core::Error { "capture.take_write_failed", "test", {} });
    f.window->handleError (overdub::core::Error { "capture.nothing_armed", "test", {} });

    CHECK (f.alertsShown.empty());
    CHECK (eventsNamed (capture, "ui.alert_deferred").size() == 1);
    const auto notices = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0].at ("code") == "capture.nothing_armed");
    CHECK (notices[0].at ("tier") == "notice");
    CHECK (isShown (f.window->noticeLine));

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    CHECK (f.alertsShown.size() == 1);
    const auto alerts = eventsNamed (capture, "ui.alert_shown");
    REQUIRE (alerts.size() == 1);
    CHECK (alerts[0].at ("code") == "capture.take_write_failed");
    CHECK (alerts[0].at ("tier") == "alert");
}

TEST_CASE ("R with nothing Armed shows a Notice and starts no Pass", "[ui][main][messages]")
{
    MainWindowFixture f;
    click (f.rowNamed ("Guitar").armButton);
    click (f.rowNamed ("Vocals").armButton);

    ScopedLogCapture capture;
    REQUIRE (pressKey (*f.window, 'r'));

    CHECK (! f.session->isRecording());
    const auto notices = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0].at ("code") == "capture.nothing_armed");
    CHECK (eventsNamed (capture, "capture.nothing_armed").size() == 1);
}

TEST_CASE ("with no Song open the window offers New Song and Open", "[ui][main]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    overdub::ui::MainComponent window;
    layOut (window, windowWidth, windowHeight);
    int newSongs = 0;
    window.onNewSong = [&] { ++newSongs; };

    CHECK (isShown (window.noSongPage));
    CHECK (! isShown (window.recordPage));
    CHECK (! window.topBar.recordButton.isEnabled());
    CHECK (pressKey (window, 'r')); // used, but there is nothing to record into

    click (window.noSongPage.newSongButton);
    CHECK (newSongs == 1);
}
