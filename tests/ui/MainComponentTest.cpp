#include "ui/MainComponent.h"

#include "ui/Layout.h"
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

std::vector<std::string> codesOf (const std::vector<overdub::ui::UserMessage>& messages)
{
    std::vector<std::string> codes;
    for (const auto& message : messages)
        codes.push_back (message.code);
    return codes;
}

overdub::ui::IntentResult dismissAlerts (MainWindowFixture& f)
{
    return f.window->run (overdub::ui::WindowIntent { overdub::ui::intent::DismissAlerts {} }, overdub::ui::Origin::control);
}

// The TooltipWindows that could show the component's tips: children of the component or of one
// of its parents, as JUCE finds them when the mouse hovers.
std::vector<juce::TooltipWindow*> tooltipWindowsFor (juce::Component& component)
{
    std::vector<juce::TooltipWindow*> found;
    for (auto* parent = &component; parent != nullptr; parent = parent->getParentComponent())
        for (auto* child : parent->getChildren())
            if (auto* tooltipWindow = dynamic_cast<juce::TooltipWindow*> (child))
                found.push_back (tooltipWindow);
    return found;
}
} // namespace

TEST_CASE ("keys 1 to 3 show the Record, Edit and Export pages", "[ui][main]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    CHECK (isShown (window.songView()->recordPage));

    CHECK (pressKey (window, '2'));
    CHECK (window.mode() == Mode::edit);
    CHECK (isShown (window.songView()->editPage));
    CHECK (! isShown (window.songView()->recordPage));
    CHECK (window.topBar.editModeButton.getToggleState());

    CHECK (pressKey (window, '3'));
    CHECK (isShown (window.songView()->exportPage));
    CHECK (! isShown (window.songView()->editPage));

    CHECK (pressKey (window, '1'));
    CHECK (isShown (window.songView()->recordPage));
    CHECK (! isShown (window.songView()->exportPage));
    CHECK (window.topBar.recordModeButton.getToggleState());

    CHECK (! pressKey (window, 'x'));
}

TEST_CASE ("clicking a mode button shows its page", "[ui][main]")
{
    MainWindowFixture f;
    click (f.window->topBar.exportModeButton);
    CHECK (isShown (f.window->songView()->exportPage));
    click (f.window->topBar.recordModeButton);
    CHECK (isShown (f.window->songView()->recordPage));
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

    CHECK (! f.openSong->isRecording());
    CHECK (f.song().passes.size() == 1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
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
    CHECK (f.openSong->isRecording());
    click (f.window->topBar.playStopButton);
    f.runBlocks (1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
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
    CHECK (f.window->songView()->recordPage.rows.size() == 1);

    click (topBar.redoButton);
    CHECK (text (topBar.undoButton) == "Undo Add Tracks");
    CHECK (f.window->songView()->recordPage.rows.size() == 3);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (text (topBar.redoButton) == "Redo Add Tracks");
    REQUIRE (pressKey (*f.window, 'Z', juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)));
    CHECK (text (topBar.undoButton) == "Undo Add Tracks");
}

TEST_CASE ("arm and the input picker change the Song without a history step", "[ui][main][rows]")
{
    MainWindowFixture f;
    const auto stepBefore = f.openSong->package().undoLabel();
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
    CHECK (f.openSong->isMonitoring (f.trackNamed ("Guitar").id));
    CHECK (f.rowNamed ("Guitar").monitoringButton.getToggleState());

    CHECK (f.openSong->package().undoLabel() == stepBefore);
    CHECK (f.openSong->package().hasPendingWrite());
}

TEST_CASE ("an injected import adds a Track row named after the file", "[ui][main][import]")
{
    MainWindowFixture f;
    const auto file = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    f.window->chooseFilesToImport = [file] (std::function<void (std::vector<juce::File>)> done) { done ({ file }); };

    REQUIRE (f.window->songView()->recordPage.rows.size() == 3);
    click (f.window->songView()->recordPage.importButton);

    REQUIRE (f.window->songView()->recordPage.rows.size() == 4);
    CHECK (f.window->songView()->recordPage.rows.back()->nameLabel.getText() == "Drums");
    CHECK (text (f.window->topBar.undoButton) == "Undo Import Drums.wav");
}

TEST_CASE ("a failed import is an Alert", "[ui][main][import][messages]")
{
    MainWindowFixture f;
    const auto notAudio = f.fixture->folder.getChildFile ("notes.txt");
    REQUIRE (notAudio.replaceWithText ("not audio"));
    f.window->chooseFilesToImport = [notAudio] (std::function<void (std::vector<juce::File>)> done) { done ({ notAudio }); };

    ScopedLogCapture capture;
    click (f.window->songView()->recordPage.importButton);

    const auto shown = eventsNamed (capture, "ui.alert_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0].at ("code") == "song.import_unreadable");
    CHECK (shown[0].at ("tier") == "alert");
    CHECK (f.alertsShown.size() == 1);
    CHECK (f.window->songView()->recordPage.rows.size() == 3);
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

TEST_CASE ("an Alert raised during a Pass is deferred, not open, until the Pass stops", "[ui][main][messages][alerts]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (4);
    f.window->handleError (overdub::core::Error { "capture.take_write_failed", "test", {} });

    CHECK (codesOf (f.window->alertsDeferred()) == std::vector<std::string> { "capture.take_write_failed" });
    CHECK (f.window->alertsShown().empty());

    ScopedLogCapture capture;
    REQUIRE (dismissAlerts (f).has_value());
    CHECK (eventsNamed (capture, "ui.alert_dismissed").empty());
    CHECK (codesOf (f.window->alertsDeferred()) == std::vector<std::string> { "capture.take_write_failed" });

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    CHECK (f.window->alertsDeferred().empty());
    CHECK (codesOf (f.window->alertsShown()) == std::vector<std::string> { "capture.take_write_failed" });
}

TEST_CASE ("dismiss_alerts closes every open Alert once, logs each, and a second one does nothing", "[ui][main][messages][alerts]")
{
    MainWindowFixture f;
    f.window->handleError (overdub::core::Error { "song.import_unreadable", "test", {} });
    f.window->handleError (overdub::core::Error { "song.write_failed", "test", {} });
    REQUIRE (codesOf (f.window->alertsShown()) == std::vector<std::string> { "song.import_unreadable", "song.write_failed" });

    ScopedLogCapture capture;
    const auto result = dismissAlerts (f);

    REQUIRE (result.has_value());
    CHECK (result->createdId == std::nullopt);
    CHECK (f.alertClosesByWindow == std::vector<int> { 1, 1 });
    CHECK (f.window->alertsShown().empty());
    const auto dismissed = eventsNamed (capture, "ui.alert_dismissed");
    REQUIRE (dismissed.size() == 2);
    CHECK (dismissed[0].at ("code") == "song.import_unreadable");
    CHECK (dismissed[0].at ("tier") == "alert");
    CHECK (dismissed[1].at ("code") == "song.write_failed");

    REQUIRE (dismissAlerts (f).has_value());
    CHECK (f.alertClosesByWindow == std::vector<int> { 1, 1 });
    CHECK (eventsNamed (capture, "ui.alert_dismissed").size() == 2);
}

TEST_CASE ("an Alert the user closes leaves the open Alerts; dismiss_alerts closes only the others, and a late OK does nothing", "[ui][main][messages][alerts]")
{
    MainWindowFixture f;
    f.window->handleError (overdub::core::Error { "song.import_unreadable", "test", {} });
    f.window->handleError (overdub::core::Error { "song.write_failed", "test", {} });

    f.closeAlertAsUser[0]();
    CHECK (codesOf (f.window->alertsShown()) == std::vector<std::string> { "song.write_failed" });

    ScopedLogCapture capture;
    REQUIRE (dismissAlerts (f).has_value());
    CHECK (f.alertClosesByWindow == std::vector<int> { 0, 1 });
    const auto dismissed = eventsNamed (capture, "ui.alert_dismissed");
    REQUIRE (dismissed.size() == 1);
    CHECK (dismissed[0].at ("code") == "song.write_failed");

    f.closeAlertAsUser[1]();
    f.closeAlertAsUser[0]();
    CHECK (f.window->alertsShown().empty());
    CHECK (eventsNamed (capture, "ui.alert_dismissed").size() == 1);
}

TEST_CASE ("R with nothing Armed shows a Notice and starts no Pass", "[ui][main][messages]")
{
    MainWindowFixture f;
    click (f.rowNamed ("Guitar").armButton);
    click (f.rowNamed ("Vocals").armButton);

    ScopedLogCapture capture;
    REQUIRE (pressKey (*f.window, 'r'));

    CHECK (! f.openSong->isRecording());
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
    CHECK (window.songView() == nullptr);
    CHECK (! window.topBar.recordButton.isEnabled());
    CHECK (pressKey (window, 'r')); // used, but there is nothing to record into

    click (window.noSongPage.newSongButton);
    CHECK (newSongs == 1);
}

TEST_CASE ("a Pass stopped before any audio arrived shows a Notice", "[ui][main][messages]")
{
    MainWindowFixture f;
    ScopedLogCapture capture;
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    CHECK (! f.openSong->isRecording());
    const auto notices = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0].at ("code") == "capture.pass_empty");
    CHECK (notices[0].at ("tier") == "notice");
    CHECK (f.alertsShown.empty());
}

TEST_CASE ("R without an audio device shows a Notice", "[ui][main][messages]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto fixture = overdub::song::testing::threeTrackFixture();
    {
        overdub::ui::OpenSong song { std::move (fixture->package), overdub::ui::AudioBinding {} };
        overdub::ui::MainComponent window;
        layOut (window, windowWidth, windowHeight);
        window.setOpenSong (&song);

        ScopedLogCapture capture;
        REQUIRE (pressKey (window, 'r'));
        const auto notices = eventsNamed (capture, "ui.notice_shown");
        REQUIRE (notices.size() == 1);
        CHECK (notices[0].at ("code") == "ui.no_audio_device");
        CHECK (notices[0].at ("tier") == "notice");
        window.setOpenSong (nullptr);
    }
}

TEST_CASE ("A on a Record row and on an Edit header arms the Track, follows Armed, and is disabled while a Pass runs", "[ui][main][arm]")
{
    MainWindowFixture f;
    CHECK (text (f.rowNamed ("Guitar").armButton) == "A");
    CHECK (f.rowNamed ("Guitar").armButton.getTooltip() == "Arm");
    REQUIRE (f.trackNamed ("Guitar").armed);
    CHECK (f.rowNamed ("Guitar").armButton.getToggleState());

    click (f.rowNamed ("Guitar").armButton);
    CHECK (! f.trackNamed ("Guitar").armed);
    CHECK (! f.rowNamed ("Guitar").armButton.getToggleState());

    REQUIRE (pressKey (*f.window, '2'));
    CHECK (text (f.editHeaderNamed ("Guitar").armButton) == "A");
    CHECK (f.editHeaderNamed ("Guitar").armButton.getTooltip() == "Arm");
    CHECK (! f.editHeaderNamed ("Guitar").armButton.getToggleState());

    click (f.editHeaderNamed ("Guitar").armButton);
    CHECK (f.trackNamed ("Guitar").armed);
    CHECK (f.rowNamed ("Guitar").armButton.getToggleState());
    CHECK (f.editHeaderNamed ("Guitar").armButton.getToggleState());

    CHECK (f.rowNamed ("Guitar").armButton.isEnabled());
    CHECK (f.editHeaderNamed ("Guitar").armButton.isEnabled());
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    CHECK_FALSE (f.rowNamed ("Guitar").armButton.isEnabled());
    CHECK_FALSE (f.editHeaderNamed ("Guitar").armButton.isEnabled());
}

TEST_CASE ("the top bar names the process controlling the app while a control client is connected", "[ui][main][control]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    const auto& indicator = window.topBar.controlIndicator;
    window.tick();
    CHECK (indicator.nameText() == "");

    window.controllingProcesses = { "python3" };
    window.tick();
    CHECK (std::string (overdub::ui::ControlIndicator::caption) == "CONTROLLED BY");
    CHECK (indicator.nameText() == "python3");
    CHECK (indicator.isVisible());
    CHECK (indicator.getRight() < window.topBar.rewindButton.getX());
    CHECK (! indicator.getBounds().intersects (window.topBar.clock.getBounds()));

    window.controllingProcesses = { "python3", "overdub-ctl", "Terminal" };
    window.tick();
    CHECK (indicator.nameText() == "python3 +2");
    CHECK (writeSnapshot (window, "issue-177", "controlled-by-three").existsAsFile());

    window.controllingProcesses = {};
    window.tick();
    CHECK (indicator.nameText() == "");
}

TEST_CASE ("the control indicator shows without a Song open, at the smallest window width, with a long name ellipsized", "[ui][main][control]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    overdub::ui::MainComponent window;
    layOut (window, overdub::ui::minimumWindowWidth, overdub::ui::minimumWindowHeight);
    window.controllingProcesses = { "overdub-ctl-agent-1234567" };
    window.tick();
    CHECK (window.topBar.controlIndicator.nameText() == "overdub-ctl-agent-1234567");
    CHECK (window.topBar.controlIndicator.getRight() < window.topBar.rewindButton.getX());
    CHECK (writeSnapshot (window, "issue-177", "controlled-long-name-minimum-width").existsAsFile());
}

// Not through TooltipWindow::getTipFor: it returns "" unless the process is in the foreground,
// which a test run is not.
TEST_CASE ("a control with a tooltip inside the window finds the window's one TooltipWindow", "[ui][main]")
{
    MainWindowFixture f;
    auto& recordArm = f.rowNamed ("Guitar").armButton;
    REQUIRE (recordArm.getTooltip().isNotEmpty());
    const auto forRecordRow = tooltipWindowsFor (recordArm);
    REQUIRE (forRecordRow.size() == 1);
    CHECK (forRecordRow.front()->getParentComponent() == f.window.get());

    REQUIRE (pressKey (*f.window, '2'));
    auto& timelineDelete = f.editHeaderNamed ("Guitar").deleteButton;
    REQUIRE (timelineDelete.getTooltip().isNotEmpty());
    CHECK (tooltipWindowsFor (timelineDelete) == forRecordRow);
}
