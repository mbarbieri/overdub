#include "ui/TranscribePage.h"

#include "core/Log.h"
#include "ui/MainWindowFixture.h"
#include "ui/RecordView.h"
#include "ui/TranscribeGeometry.h"
#include "ui/WindowSettings.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;
using core::ScopedLogCapture;

// Transcribe mode (issue #225, ADR 0010): the third mode's page, an overview over one tall,
// zoomable waveform of the heard mix, and a right column with Practice Speed and Transpose that I
// shows or hides. Driven through MainComponent::run, keys and clicks on the fixture Song (a 2 s
// Backing Track "Band Mix", a 0.25 sine, and the Tracks Guitar and Vocals).
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

IntentResult runControl (MainWindowFixture& f, const Intent& intent)
{
    return f.window->run (intent, Origin::control);
}

TranscribePage& showTranscribe (MainWindowFixture& f)
{
    REQUIRE (runControl (f, WindowIntent { intent::ShowMode { Mode::transcribe } }).has_value());
    return f.window->songView()->transcribePage;
}

void wheel (juce::Component& target, float deltaX, float deltaY, juce::ModifierKeys modifiers = {})
{
    const juce::MouseWheelDetails details { deltaX, deltaY, false, false, false };
    target.mouseWheelMove (mouseEventAt (target, target.getLocalBounds().getCentre().toFloat(), modifiers), details);
}

double visibleSeconds (const TranscribePage& page, int sampleRate)
{
    return (double) page.view().samplesPerPixel * page.wave.getWidth() / sampleRate;
}

juce::File temporarySettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-window-settings-" + juce::Uuid().toString() + ".json");
}
} // namespace

TEST_CASE ("3 and the Transcribe button show Transcribe mode, named transcribe in ui events", "[ui][transcribe][mode]")
{
    MainWindowFixture f;
    auto& window = *f.window;
    const ScopedLogCapture capture;

    CHECK (pressKey (window, '3'));
    CHECK (window.mode() == Mode::transcribe);
    CHECK (isShown (window.songView()->transcribePage));
    CHECK_FALSE (isShown (window.songView()->recordPage));
    CHECK_FALSE (isShown (window.songView()->editPage));
    CHECK (window.topBar.transcribeModeButton.getToggleState());
    CHECK_FALSE (window.topBar.recordModeButton.getToggleState());

    const auto changed = eventsNamed (capture, "ui.mode_changed");
    REQUIRE (changed.size() == 1);
    CHECK (changed[0].at ("mode") == "transcribe");

    click (window.topBar.recordModeButton);
    CHECK_FALSE (isShown (window.songView()->transcribePage));
    click (window.topBar.transcribeModeButton);
    CHECK (window.mode() == Mode::transcribe);
    CHECK (isShown (window.songView()->transcribePage));
}

TEST_CASE ("the waveform and the overview draw the heard Transcribe Mix", "[ui][transcribe][page]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    const auto sampleRate = f.song().sampleRate;

    const auto& columns = page.waveformColumns();
    REQUIRE ((int) columns.size() == page.wave.getWidth());
    const auto view = page.view();
    // The 0.25 sine plays for 2 s from 0; after it nothing sounds.
    CHECK (columns.at (10).maximum == Catch::Approx (0.25f).margin (0.01f));
    CHECK (columns.at (10).minimum == Catch::Approx (-0.25f).margin (0.01f));
    const auto afterBacking = (std::size_t) (3 * sampleRate / view.samplesPerPixel);
    REQUIRE (afterBacking < columns.size());
    CHECK (columns.at (afterBacking).maximum == Catch::Approx (0.0f));

    SECTION ("a Backing Track muted in the Transcribe Mix draws nothing")
    {
        REQUIRE (runControl (f, SongIntent { intent::SetTranscribeMute { f.trackNamed ("Band Mix").id, true } }).has_value());
        CHECK (page.waveformColumns().at (10).maximum == Catch::Approx (0.0f));
    }
}

TEST_CASE ("clicking or dragging the overview moves the visible window there", "[ui][transcribe][overview]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    const auto length = overviewLength (f.song(), f.openSong->position());
    const auto samplesPerPixel = page.view().samplesPerPixel;
    const auto visible = samplesPerPixel * page.wave.getWidth();
    const auto width = page.overview.getWidth();
    const auto intentsBefore = f.window->intentsRun();

    clickAt (page.overview, { width / 2, 10 });
    CHECK (f.window->intentsRun() > intentsBefore);
    CHECK (page.view().firstSample == overviewSampleAt (width / 2, width, length) - visible / 2);
    CHECK (page.view().samplesPerPixel == samplesPerPixel);
    // The overview's rectangle is the visible window.
    CHECK (page.overview.windowRectangle().getX() == Catch::Approx (overviewX (page.view().firstSample, width, length)));
    CHECK (page.overview.windowRectangle().getRight() == Catch::Approx (overviewX (page.view().firstSample + visible, width, length)));

    drag (page.overview, { width / 2, 10 }, { width / 4, 10 });
    CHECK (page.view().firstSample == overviewSampleAt (width / 4, width, length) - visible / 2);

    drag (page.overview, { width / 4, 10 }, { 0, 10 });
    CHECK (page.view().firstSample == 0);
    drag (page.overview, { 0, 10 }, { width, 10 });
    CHECK (page.view().firstSample == length - visible);
}

TEST_CASE ("the wheel scrolls, and command-wheel or a pinch zooms in until about 2 s fills the width", "[ui][transcribe][zoom]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    const auto sampleRate = f.song().sampleRate;
    const auto start = page.view();

    wheel (page.wave, 0.0f, -0.1f);
    CHECK (page.view().firstSample == start.firstSample + 40 * start.samplesPerPixel);
    wheel (page.wave, 0.1f, 0.0f);
    CHECK (page.view().firstSample == start.firstSample);

    for (int i = 0; i < 12; ++i)
        wheel (page.wave, 0.0f, 0.2f, juce::ModifierKeys::commandModifier);
    const auto finest = page.view().samplesPerPixel;
    CHECK (finest < start.samplesPerPixel);
    CHECK (visibleSeconds (page, sampleRate) > finestTranscribeSeconds / 1.5);
    CHECK (visibleSeconds (page, sampleRate) < finestTranscribeSeconds * 1.5);
    page.wave.mouseMagnify (mouseEventAt (page.wave, { 100.0f, 50.0f }, {}), 4.0f);
    CHECK (page.view().samplesPerPixel == finest);

    for (int i = 0; i < 12; ++i)
        page.wave.mouseMagnify (mouseEventAt (page.wave, { 100.0f, 50.0f }, {}), 0.25f);
    // Zoomed out, the whole Song fits.
    const auto length = overviewLength (f.song(), f.openSong->position());
    CHECK (page.view().samplesPerPixel * page.wave.getWidth() >= length);
    CHECK (page.view().firstSample == 0);
}

TEST_CASE ("set_transcribe_view from the socket is clamped as the gestures are", "[ui][transcribe][zoom]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    REQUIRE (runControl (f, SongIntent { intent::SetTranscribeView { -100, 1 } }).has_value());
    CHECK (page.view().firstSample == 0);
    CHECK (page.view().samplesPerPixel == transcribeMinSamplesPerPixel (f.song().sampleRate, page.wave.getWidth()));
}

TEST_CASE ("clicking the ruler or the waveform seeks, and Space plays and stops", "[ui][transcribe][transport]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    const auto view = page.view();

    clickAt (page.wave, { 100, 5 }); // the ruler
    CHECK (f.openSong->position() == view.firstSample + 100 * view.samplesPerPixel);
    clickAt (page.wave, { 40, page.wave.getHeight() / 2 }); // the waveform
    CHECK (f.openSong->position() == view.firstSample + 40 * view.samplesPerPixel);

    CHECK (pressKey (*f.window, ' '));
    f.runBlocks (1);
    CHECK (f.engine.isPlaying());
    CHECK (pressKey (*f.window, ' '));
    f.runBlocks (1);
    CHECK_FALSE (f.engine.isPlaying());
}

TEST_CASE ("the Transcribe column's Practice Speed and Transpose run the top bar's Intents", "[ui][transcribe][column]")
{
    MainWindowFixture f;
    auto& page = showTranscribe (f);
    REQUIRE (isShown (page.column));
    CHECK (page.column.practiceSpeedField.getText() == "100");
    CHECK (page.column.transposeField.getText() == "0");
    const ScopedLogCapture capture;

    page.column.practiceSpeedField.setText ("80", juce::sendNotificationSync);
    CHECK (f.openSong->listening().practiceSpeed() == 80);
    CHECK (page.column.practiceSpeedField.getText() == "80");
    CHECK (f.window->topBar.practiceSpeedField.getText() == "80");

    page.column.transposeField.setText ("+2", juce::sendNotificationSync);
    CHECK (f.song().transpose == overdub::song::Transpose { .semitones = 2 });
    CHECK (f.window->topBar.transposeField.getText() == "+2");

    // The fine tune (issue #232): its own field beside "st", keeping the semitones, and one undo step.
    CHECK (page.column.transposeCentsField.getText() == "0");
    CHECK (page.column.transposeCentsUnit.getText() == "ct");
    page.column.transposeCentsField.setText ("-30", juce::sendNotificationSync);
    CHECK (f.song().transpose == overdub::song::Transpose { .semitones = 2, .cents = -30 });
    CHECK (page.column.transposeCentsField.getText() == juce::String::fromUTF8 ("\xe2\x88\x92"
                                                                                "30"));
    CHECK (page.column.transposeField.getText() == "+2");
    page.column.transposeField.setText ("3", juce::sendNotificationSync);
    CHECK (f.song().transpose == overdub::song::Transpose { .semitones = 3, .cents = -30 });
    REQUIRE (f.openSong->undo().has_value());
    REQUIRE (f.openSong->undo().has_value());
    f.window->refresh();
    CHECK (f.song().transpose == overdub::song::Transpose { .semitones = 2 });
    CHECK (page.column.transposeCentsField.getText() == "0");

    page.column.practiceSpeedField.setText ("fast", juce::sendNotificationSync);
    CHECK (f.openSong->listening().practiceSpeed() == 80);
    CHECK (page.column.practiceSpeedField.getText() == "80");

    std::vector<std::string> ops;
    for (const auto& event : eventsNamed (capture, "ui.intent_run"))
        ops.push_back (event.at ("op"));
    CHECK (ops == std::vector<std::string> { "set_practice_speed", "set_transpose", "set_transpose", "set_transpose" });
}

TEST_CASE ("I shows and hides the Transcribe column, remembered per Mac and not in the Song", "[ui][transcribe][column]")
{
    const auto settingsFile = temporarySettingsFile();
    {
        MainWindowFixture f;
        f.window->useWindowSettingsFile (settingsFile);
        auto& page = showTranscribe (f);
        const auto songBefore = f.song();
        const auto narrowWave = page.wave.getWidth();
        REQUIRE (f.window->isTranscribeColumnVisible());

        CHECK (pressKey (*f.window, 'i'));
        CHECK_FALSE (f.window->isTranscribeColumnVisible());
        CHECK_FALSE (isShown (page.column));
        CHECK (page.wave.getWidth() > narrowWave);
        auto read = readWindowSettings (settingsFile);
        REQUIRE (read.has_value());
        CHECK_FALSE (read->transcribeColumnVisible);
        CHECK (f.song() == songBefore);

        CHECK (pressKey (*f.window, 'I'));
        CHECK (isShown (page.column));
        CHECK (page.wave.getWidth() == narrowWave);

        REQUIRE (runControl (f, WindowIntent { intent::SetTranscribeColumnVisible { false } }).has_value());
        CHECK_FALSE (isShown (page.column));
    }
    {
        // The next launch reads it back.
        MainWindowFixture f;
        f.window->useWindowSettingsFile (settingsFile);
        auto& page = showTranscribe (f);
        CHECK_FALSE (f.window->isTranscribeColumnVisible());
        CHECK_FALSE (isShown (page.column));
    }
    settingsFile.deleteFile();
}

TEST_CASE ("in Transcribe mode the Clip edits and nudge are refused with ui.transcribing and change nothing", "[ui][transcribe][availability]")
{
    MainWindowFixture f;
    const auto clipId = f.trackNamed ("Band Mix").clips.front().id;
    REQUIRE (runControl (f, SongIntent { intent::SelectClip { clipId } }).has_value());
    REQUIRE (runControl (f, SongIntent { intent::Seek { 4800 } }).has_value());
    showTranscribe (f);
    const auto before = f.song();

    const std::vector<SongIntent> refused {
        intent::SplitSelectedClip {},
        intent::DeleteSelection {},
        intent::Nudge { Direction::forward },
        intent::Edit { intent::MoveClip { clipId, 9600 } },
        intent::Edit { intent::SetClipGain { clipId, -3.0 } },
    };
    for (const auto& songIntent : refused)
    {
        const auto result = runControl (f, songIntent);
        REQUIRE_FALSE (result.has_value());
        CHECK (result.error().code == "ui.transcribing");
        CHECK (result.error().fields.at ("op") == std::string (opName (Intent { songIntent })));
    }
    CHECK (f.song() == before);
    CHECK (f.openSong->position() == 4800);
    // The keys too.
    CHECK (pressKey (*f.window, 's'));
    CHECK (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (f.song() == before);

    // Edit mode still splits.
    REQUIRE (runControl (f, WindowIntent { intent::ShowMode { Mode::edit } }).has_value());
    CHECK (runControl (f, SongIntent { intent::SplitSelectedClip {} }).has_value());
}
