#include "ui/RecordPage.h"

#include "song/MixEdits.h"
#include "song/TakeEdits.h"
#include "ui/MainWindowFixture.h"
#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <string>

// Each Record row's horizontal volume fader (issue #130, ADR 0005 UI amendment) through the real
// window: synthesized drags, double-clicks and key presses on it, then the Song, the undo step's
// label, what undo restores and what the Mixer fader shows. Expectations come from the Mixer's
// pure mappings (ui/MixerGeometry.h).
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;

namespace
{
struct RecordFaderFixture : MainWindowFixture
{
    RecordFaderFixture()
    {
        REQUIRE (mode() == Mode::record);
        REQUIRE (isShown (window->recordPage));
    }

    Mode mode() const { return window->mode(); }

    MixerFader& faderOf (const std::string& name) { return rowNamed (name).fader; }

    MixerStrip& stripNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& strip : window->editPage.mixer.strips)
            if (strip->trackId() == trackId)
                return *strip;
        FAIL ("no strip for Track " << name);
        return *window->editPage.mixer.strips.front();
    }

    MixFocus& focus() { return window->recordPage.mix; }

    std::string undoLabel() const { return openSong->package().undoLabel(); }

    void undo() { REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier)); }

    void useFixedClock()
    {
        now = overdub::song::SongPackage::Clock::now();
        window->clock = [this] { return now; };
    }

    bool press (int keyCode, juce::ModifierKeys modifiers = {}) { return pressKey (*window, juce::KeyPress (keyCode, modifiers, 0)); }

    // A mouse down on `component` as the window's listener sees it (a real click reaches both).
    void mouseDownOn (juce::Component& component)
    {
        window->mouseDown (mouseEventAt (component, component.getLocalBounds().getCentre().toFloat(), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
    }

    void expectRowShows (const std::string& name, double db)
    {
        CHECK_THAT (faderOf (name).db(), WithinAbs (db, 1.0e-9));
        CHECK (rowNamed (name).volumeLabel.getText() == juce::String (volumeText (db)));
    }

    overdub::song::SongPackage::Clock::time_point now;
};

constexpr auto shift = juce::ModifierKeys::shiftModifier;
using std::chrono::milliseconds;
} // namespace

TEST_CASE ("each Record row shows a horizontal fader and dB text matching its Track's volume", "[ui][record_fader]")
{
    RecordFaderFixture f;
    for (const auto& track : f.song().tracks)
    {
        auto& row = f.rowNamed (track.name);
        CHECK (isShown (row.fader));
        CHECK (isShown (row.volumeLabel));
        CHECK (row.fader.getWidth() > 4 * row.fader.getHeight());
        CHECK (row.volumeLabel.getX() > row.fader.getRight());
        f.expectRowShows (track.name, track.volumeDb);
    }

    // A Track at another volume shows it once the Song changes.
    auto changed = overdub::song::setTrackVolume (f.song(), f.trackNamed ("Vocals").id, -12.5);
    REQUIRE (changed.has_value());
    REQUIRE (f.openSong->edit (std::move (*changed), "Vocals Volume").has_value());
    f.window->tick();
    f.expectRowShows ("Vocals", -12.5);
}

TEST_CASE ("a Record fader's thumb sits on the Mixer's fader law: 0 dB at three quarters of the travel from the left", "[ui][record_fader]")
{
    RecordFaderFixture f;
    auto& fader = f.faderOf ("Guitar");
    REQUIRE (fader.travel() > 100);
    REQUIRE_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (0.0, 1.0e-9));
    const auto thumbAt = [&fader] (double position) { return MixerFader::thumbSize / 2 + (int) std::lround (position * fader.travel()); };
    CHECK (fader.thumbCentre().x == thumbAt (faderUnityPosition));
    CHECK_THAT (faderUnityPosition, WithinAbs (0.75, 1.0e-9));

    for (const auto db : { -60.0, -30.0, -6.0, 6.0, 12.0 })
    {
        fader.setDb (db);
        CHECK (fader.thumbCentre().x == thumbAt (dbToFaderPosition (db)));
    }
    fader.setDb (0.0);
}

TEST_CASE ("dragging a Record fader sets the Track's volume as one undo step, and the Mixer fader shows it", "[ui][record_fader]")
{
    RecordFaderFixture f;
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.faderOf ("Guitar");
    const auto grab = fader.thumbCentre();
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);

    // Down and two moves left: nothing is committed yet, the row shows the dragged dB.
    fader.mouseDown (mouseEventAt (fader, grab.toFloat(), leftButton));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (-10, 0).toFloat(), leftButton, grab.toFloat()));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (-40, 0).toFloat(), leftButton, grab.toFloat()));
    f.window->tick(); // a poll during the drag keeps the dragged value shown
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    const auto expectedDb = faderDragDb (0.0, -40, fader.travel());
    REQUIRE (expectedDb < -3.0);
    f.expectRowShows ("Guitar", expectedDb);

    fader.mouseUp (mouseEventAt (fader, grab.translated (-40, 0).toFloat(), {}, grab.toFloat()));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (expectedDb, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Volume");

    REQUIRE (pressKey (*f.window, '2'));
    CHECK_THAT (f.stripNamed ("Guitar").fader.db(), WithinAbs (expectedDb, 1.0e-9));

    f.undo();
    CHECK (f.song() == before);
    CHECK_THAT (f.stripNamed ("Guitar").fader.db(), WithinAbs (0.0, 1.0e-9));
    REQUIRE (pressKey (*f.window, '1'));
    f.expectRowShows ("Guitar", 0.0);
}

TEST_CASE ("dragging a Mixer fader moves the Record fader to the same volume", "[ui][record_fader]")
{
    RecordFaderFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    auto& mixerFader = f.stripNamed ("Vocals").fader;
    drag (mixerFader, mixerFader.thumbCentre(), mixerFader.thumbCentre().translated (0, 30));
    const auto expectedDb = faderDragDb (0.0, -30, mixerFader.travel());
    REQUIRE (expectedDb < -3.0);
    CHECK_THAT (f.trackNamed ("Vocals").volumeDb, WithinAbs (expectedDb, 1.0e-9));

    REQUIRE (pressKey (*f.window, '1'));
    f.expectRowShows ("Vocals", expectedDb);
}

TEST_CASE ("a Record fader drag sticks at 0 dB for the detent from either side, and one that ends where it started changes nothing", "[ui][record_fader]")
{
    RecordFaderFixture f;
    auto& fader = f.faderOf ("Guitar");
    const auto travel = fader.travel();

    // From below: a drag that just reaches 0 dB, plus most of the detent, lands on 0 dB.
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (-40, 0));
    const auto below = f.trackNamed ("Guitar").volumeDb;
    REQUIRE (below < 0.0);
    const auto toUnity = (int) std::lround ((faderUnityPosition - dbToFaderPosition (below)) * travel);
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (toUnity + detentPixels - 1, 0));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (faderDragDb (below, toUnity + detentPixels - 1, travel), 1.0e-9));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (0.0, 1.0e-9));

    // From 0 dB a drag moves at once, louder to the right.
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (20, 0));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (faderDragDb (0.0, 20, travel), 1.0e-9));
    CHECK (f.trackNamed ("Guitar").volumeDb > 0.0);

    // From above, back past 0 dB within the detent: it lands on 0 dB.
    const auto above = f.trackNamed ("Guitar").volumeDb;
    const auto backToUnity = (int) std::lround ((dbToFaderPosition (above) - faderUnityPosition) * travel);
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (-(backToUnity + detentPixels - 1), 0));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (faderDragDb (above, -(backToUnity + detentPixels - 1), travel), 1.0e-9));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (0.0, 1.0e-9));

    const auto settled = f.song();
    const auto label = f.undoLabel();
    const auto grab = fader.thumbCentre();
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);
    fader.mouseDown (mouseEventAt (fader, grab.toFloat(), leftButton));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (30, 0).toFloat(), leftButton, grab.toFloat()));
    fader.mouseDrag (mouseEventAt (fader, grab.toFloat(), leftButton, grab.toFloat()));
    fader.mouseUp (mouseEventAt (fader, grab.toFloat(), {}, grab.toFloat()));
    CHECK (f.song() == settled);
    CHECK (f.undoLabel() == label);
}

TEST_CASE ("double-clicking a Record fader sets 0.0 dB as one undo step, none when already there", "[ui][record_fader]")
{
    RecordFaderFixture f;
    auto& fader = f.faderOf ("Vocals");
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (-50, 0));
    REQUIRE (f.trackNamed ("Vocals").volumeDb < -5.0);
    const auto dragged = f.song();

    doubleClickAt (fader, fader.thumbCentre());
    CHECK_THAT (f.trackNamed ("Vocals").volumeDb, WithinAbs (0.0, 1.0e-9));
    f.expectRowShows ("Vocals", 0.0);
    CHECK (f.undoLabel() == "Vocals Volume");
    f.undo();
    CHECK (f.song() == dragged);
    f.undo();

    const auto atUnity = f.song();
    const auto labelAtUnity = f.undoLabel();
    doubleClickAt (fader, fader.thumbCentre());
    CHECK (f.song() == atUnity);
    CHECK (f.undoLabel() == labelAtUnity);
}

TEST_CASE ("after clicking a Record fader the arrow keys move it by 1 dB, 0.1 with shift, stopping at 0 dB", "[ui][record_fader][keys]")
{
    RecordFaderFixture f;
    f.useFixedClock();
    auto& fader = f.faderOf ("Guitar");
    CHECK (! f.press (juce::KeyPress::upKey)); // nothing has focus: the window does nothing with it

    clickAt (fader, fader.thumbCentre());
    REQUIRE (f.focus().focusedControl() == MixerControl { f.trackNamed ("Guitar").id, MixerControlKind::volume });
    CHECK (fader.isFocusShown());

    CHECK (f.press (juce::KeyPress::upKey));
    f.expectRowShows ("Guitar", 1.0);
    CHECK (f.press (juce::KeyPress::rightKey));
    f.expectRowShows ("Guitar", 2.0);
    CHECK (f.press (juce::KeyPress::downKey));
    f.expectRowShows ("Guitar", 1.0);
    CHECK (f.press (juce::KeyPress::leftKey));
    f.expectRowShows ("Guitar", 0.0);
    for (int i = 0; i < 3; ++i)
        CHECK (f.press (juce::KeyPress::downKey, shift));
    f.expectRowShows ("Guitar", -0.3);
    f.window->tick(); // a poll shows the burst's value, not the Song's
    f.expectRowShows ("Guitar", -0.3);
    CHECK (f.press (juce::KeyPress::upKey));
    f.expectRowShows ("Guitar", 0.0);
    CHECK (f.press (juce::KeyPress::upKey, shift));
    f.expectRowShows ("Guitar", 0.1);
}

TEST_CASE ("while a Record fader has focus the arrows do not move the playhead; Esc or a click elsewhere releases it", "[ui][record_fader][keys]")
{
    RecordFaderFixture f;
    f.useFixedClock();
    auto& fader = f.faderOf ("Guitar");
    REQUIRE (f.openSong->position() == 0);

    clickAt (fader, fader.thumbCentre());
    f.mouseDownOn (fader); // the window's listener sees the click on the focused fader: it keeps focus
    REQUIRE (f.focus().focusedControl().has_value());
    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK (f.press (juce::KeyPress::leftKey));
    CHECK (f.openSong->position() == 0);

    CHECK (f.press (juce::KeyPress::escapeKey));
    CHECK (! f.focus().focusedControl().has_value());
    CHECK (! fader.isFocusShown());
    const auto released = f.song();
    f.press (juce::KeyPress::upKey);
    f.press (juce::KeyPress::rightKey);
    f.window->tick (f.now + std::chrono::seconds (2));
    CHECK (f.song() == released);
    f.expectRowShows ("Guitar", f.trackNamed ("Guitar").volumeDb);

    clickAt (fader, fader.thumbCentre());
    REQUIRE (f.focus().focusedControl().has_value());
    f.mouseDownOn (f.window->recordPage.overview);
    CHECK (! f.focus().focusedControl().has_value());
    CHECK (! fader.isFocusShown());

    // Other keys still reach the window while it has focus.
    clickAt (fader, fader.thumbCentre());
    CHECK (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.openSong->isPlaying());
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("a burst of arrow presses on a Record fader is one undo step once 1 s passes with no press", "[ui][record_fader][keys]")
{
    RecordFaderFixture f;
    f.useFixedClock();
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.faderOf ("Guitar");
    clickAt (fader, fader.thumbCentre());

    const auto start = f.now;
    CHECK (f.press (juce::KeyPress::downKey));
    f.now = start + milliseconds (500);
    CHECK (f.press (juce::KeyPress::downKey));
    f.now = start + milliseconds (900);
    CHECK (f.press (juce::KeyPress::downKey));

    f.window->tick (start + milliseconds (1899)); // 999 ms after the last press
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    f.expectRowShows ("Guitar", -3.0);

    f.window->tick (start + milliseconds (1900));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (-3.0, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Volume");

    f.undo();
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    f.expectRowShows ("Guitar", 0.0);
}

TEST_CASE ("a burst on a Record fader is committed at once by focus loss, a mode change, another edit, undo or a Pass starting", "[ui][record_fader][keys]")
{
    RecordFaderFixture f;
    f.useFixedClock();
    const auto guitarId = f.trackNamed ("Guitar").id;
    auto& fader = f.faderOf ("Guitar");
    const auto startBurst = [&] {
        clickAt (fader, fader.thumbCentre());
        REQUIRE (f.press (juce::KeyPress::downKey));
        REQUIRE (f.press (juce::KeyPress::downKey));
    };
    const auto guitarDb = [&] { return overdub::song::findTrack (f.song(), guitarId)->volumeDb; };

    SECTION ("Esc")
    {
        startBurst();
        REQUIRE (f.press (juce::KeyPress::escapeKey));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("a click elsewhere")
    {
        startBurst();
        f.mouseDownOn (f.window->recordPage.overview);
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("a mode change, after which the Mixer shows it")
    {
        startBurst();
        REQUIRE (pressKey (*f.window, '2'));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
        CHECK (! f.focus().focusedControl().has_value());
        CHECK_THAT (f.stripNamed ("Guitar").fader.db(), WithinAbs (-2.0, 1.0e-9));
    }
    SECTION ("another edit keeps the burst as its own step before it")
    {
        startBurst();
        click (f.rowNamed ("Vocals").muteButton);
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.trackNamed ("Vocals").mute);
        CHECK (f.undoLabel() == "Mute Vocals");
        f.undo();
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("undo commits the burst, then undoes all of it")
    {
        const auto before = f.song();
        const auto labelBefore = f.undoLabel();
        startBurst();
        f.undo();
        CHECK (f.song() == before);
        CHECK (f.undoLabel() == labelBefore);
        CHECK (f.openSong->package().redoLabel() == "Guitar Volume");
        f.expectRowShows ("Guitar", 0.0);
    }
    SECTION ("a Pass starting")
    {
        startBurst();
        REQUIRE (pressKey (*f.window, 'r'));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
        CHECK (! f.focus().focusedControl().has_value());
        REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey)); // a Pass ends before the window closes
        f.runBlocks (1);
    }
}

TEST_CASE ("while a Pass runs, pre-roll included, the Record fader is dimmed and ignores drags, double-clicks and keys", "[ui][record_fader][keys][record]")
{
    RecordFaderFixture f;
    f.useFixedClock();
    auto& fader = f.faderOf ("Guitar");
    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (-40, 0));
    REQUIRE (f.focus().focusedControl().has_value());

    choose (f.window->recordPage.preRollPicker, preRollChoiceIndex (overdub::song::PreRoll::twoSeconds));
    f.openSong->seek (3 * f.song().sampleRate);
    f.runBlocks (0);
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (1);
    REQUIRE (f.openSong->isRecording());
    REQUIRE (f.openSong->preRollRecordPoint().has_value());
    REQUIRE (f.openSong->position() < *f.openSong->preRollRecordPoint()); // still in the pre-roll
    CHECK (! fader.isEnabled());
    CHECK (! f.focus().focusedControl().has_value());

    const auto before = f.song();
    const auto shownDb = fader.db();
    const auto grab = fader.thumbCentre();
    drag (fader, grab, grab.translated (60, 0));
    fader.mouseDoubleClick (mouseEventAt (fader, grab.toFloat(), {}));
    f.press (juce::KeyPress::upKey);
    f.press (juce::KeyPress::rightKey);
    f.now += std::chrono::seconds (2);
    f.window->tick (f.now);
    CHECK (f.song().tracks == before.tracks);
    CHECK_THAT (fader.db(), WithinAbs (shownDb, 1.0e-9));
    CHECK (! f.focus().focusedControl().has_value());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (fader.isEnabled());
}
