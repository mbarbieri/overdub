#include "ui/Mixer.h"

#include "song/TakeEdits.h"
#include "ui/MainWindowFixture.h"
#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <string>

// The Mixer through the real window (issue #31, ADR 0005 UI amendment): synthesized drags and
// clicks on its strips, the Record rows and the Edit headers, then the Song, the undo step's label
// and what undo restores.
using namespace overdub::ui;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;

namespace
{
struct MixerFixture : MainWindowFixture
{
    MixerFixture()
    {
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (isShown (mixer()));
    }

    Mixer& mixer() { return window->songView()->editPage.mixer; }

    MixerStrip& stripNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& strip : mixer().strips)
            if (strip->trackId() == trackId)
                return *strip;
        FAIL ("no strip for Track " << name);
        return *mixer().strips.front();
    }

    TrackHeader& headerNamed (const std::string& name)
    {
        const auto& trackId = trackNamed (name).id;
        for (auto& header : window->songView()->editPage.timeline.headers)
            if (header->trackId() == trackId)
                return *header;
        FAIL ("no header for Track " << name);
        return *window->songView()->editPage.timeline.headers.front();
    }

    std::string undoLabel() const { return openSong->package().undoLabel(); }

    void undo() { REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier)); }

    // Key presses happen at `now`, which the tests move by hand.
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

    overdub::song::SongPackage::Clock::time_point now;
};

constexpr auto shift = juce::ModifierKeys::shiftModifier;
using std::chrono::milliseconds;

} // namespace

TEST_CASE ("the Mixer shows one strip per Track, in Song order, and the Master strip", "[ui][mixer]")
{
    MixerFixture f;
    REQUIRE (f.mixer().strips.size() == f.song().tracks.size());
    for (std::size_t i = 0; i < f.song().tracks.size(); ++i)
    {
        CHECK (f.mixer().strips[i]->trackId() == f.song().tracks[i].id);
        CHECK (f.mixer().strips[i]->nameLabel.getText() == juce::String (f.song().tracks[i].name));
        CHECK (isShown (*f.mixer().strips[i]));
    }
    CHECK (isShown (f.mixer().masterStrip));
    CHECK (f.mixer().masterStrip.nameLabel.getText() == "Master");
    CHECK (! isShown (f.mixer().masterStrip.muteButton));
    CHECK (! isShown (f.mixer().masterStrip.panControl));
    CHECK (f.mixer().masterStrip.getRight() > f.mixer().strips.back()->getRight());
}

TEST_CASE ("a fader drag is previewed while it moves and becomes one undo step with the dragged dB", "[ui][mixer]")
{
    MixerFixture f;
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.stripNamed ("Guitar").fader;
    REQUIRE (fader.travel() > 40);
    const auto grab = fader.thumbCentre();
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);

    // Down, two moves (10 px up, then 30 px up from the grab): nothing is committed yet.
    fader.mouseDown (mouseEventAt (fader, grab.toFloat(), leftButton));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (0, -10).toFloat(), leftButton, grab.toFloat()));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (0, -30).toFloat(), leftButton, grab.toFloat()));
    f.window->tick(); // a poll during the drag keeps the dragged value shown
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    const auto expectedDb = faderDragDb (0.0, 30, fader.travel());
    CHECK_THAT (fader.db(), WithinAbs (expectedDb, 1.0e-9));
    CHECK (f.stripNamed ("Guitar").valueLabel.getText() == juce::String (volumeText (expectedDb)));
    REQUIRE (expectedDb > 3.0);

    fader.mouseUp (mouseEventAt (fader, grab.translated (0, -30).toFloat(), {}, grab.toFloat()));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (expectedDb, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Volume");

    f.undo();
    CHECK (f.song() == before);
    CHECK_THAT (fader.db(), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("dragging the Master fader down sets the Master volume as one Master Volume step", "[ui][mixer]")
{
    MixerFixture f;
    auto& fader = f.mixer().masterStrip.fader;
    const auto grab = fader.thumbCentre();
    drag (fader, grab, grab.translated (0, 40));

    CHECK_THAT (f.song().master.volumeDb, WithinAbs (faderDragDb (0.0, -40, fader.travel()), 1.0e-9));
    CHECK (f.song().master.volumeDb < -5.0);
    CHECK (f.undoLabel() == "Master Volume");
}

TEST_CASE ("dragging the pan control sets the Track's pan as one Pan step", "[ui][mixer]")
{
    MixerFixture f;
    auto& pan = f.stripNamed ("Vocals").panControl;
    const auto centre = pan.getLocalBounds().getCentre();
    drag (pan, centre, centre.translated (-pan.getWidth() / 3, 0));

    CHECK_THAT (f.trackNamed ("Vocals").pan, WithinAbs (panDrag (0.0, -pan.getWidth() / 3, pan.getWidth()), 1.0e-9));
    CHECK (f.trackNamed ("Vocals").pan < -0.4);
    CHECK (f.undoLabel() == "Vocals Pan");
}

TEST_CASE ("a fader or pan drag that ends where it started changes nothing", "[ui][mixer]")
{
    MixerFixture f;
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.stripNamed ("Guitar").fader;
    const auto grab = fader.thumbCentre();
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);
    fader.mouseDown (mouseEventAt (fader, grab.toFloat(), leftButton));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (0, -20).toFloat(), leftButton, grab.toFloat()));
    fader.mouseDrag (mouseEventAt (fader, grab.toFloat(), leftButton, grab.toFloat()));
    fader.mouseUp (mouseEventAt (fader, grab.toFloat(), {}, grab.toFloat()));

    auto& pan = f.stripNamed ("Guitar").panControl;
    clickAt (pan, pan.getLocalBounds().getCentre());

    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
}

TEST_CASE ("M and S on a Mixer strip toggle the Track's mute and solo, each one undo step", "[ui][mixer]")
{
    MixerFixture f;
    const auto before = f.song();
    auto& strip = f.stripNamed ("Band Mix");

    click (strip.muteButton);
    CHECK (f.trackNamed ("Band Mix").mute);
    CHECK (f.undoLabel() == "Mute Band Mix");
    CHECK (strip.muteButton.getToggleState());

    click (strip.soloButton);
    CHECK (f.trackNamed ("Band Mix").solo);
    CHECK (f.undoLabel() == "Solo Band Mix");

    click (strip.muteButton);
    CHECK (! f.trackNamed ("Band Mix").mute);
    CHECK (f.undoLabel() == "Unmute Band Mix");

    f.undo();
    f.undo();
    f.undo();
    CHECK (f.song() == before);
}

TEST_CASE ("M and S in the Edit Track headers toggle the Song, and the Mixer strip follows", "[ui][mixer][edit]")
{
    MixerFixture f;
    auto& header = f.headerNamed ("Guitar");
    REQUIRE (isShown (header.muteButton));
    click (header.muteButton);
    CHECK (f.trackNamed ("Guitar").mute);
    CHECK (f.undoLabel() == "Mute Guitar");
    CHECK (header.muteButton.getToggleState());
    CHECK (f.stripNamed ("Guitar").muteButton.getToggleState());

    click (header.soloButton);
    CHECK (f.trackNamed ("Guitar").solo);
    CHECK (f.undoLabel() == "Solo Guitar");
    CHECK (f.stripNamed ("Guitar").soloButton.getToggleState());
}

TEST_CASE ("M and S in the Record rows toggle the Song", "[ui][mixer][record]")
{
    MainWindowFixture f;
    auto& row = f.rowNamed ("Vocals");
    REQUIRE (isShown (row.soloButton));
    click (row.soloButton);
    CHECK (f.trackNamed ("Vocals").solo);
    CHECK (f.openSong->package().undoLabel() == "Solo Vocals");
    CHECK (row.soloButton.getToggleState());

    click (row.muteButton);
    CHECK (f.trackNamed ("Vocals").mute);
    CHECK (f.openSong->package().undoLabel() == "Mute Vocals");

    click (row.soloButton);
    CHECK (! f.trackNamed ("Vocals").solo);
    CHECK (f.openSong->package().undoLabel() == "Unsolo Vocals");
}

TEST_CASE ("the Mixer cannot be changed while a Pass records, but its meters still move", "[ui][mixer][record]")
{
    MixerFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (20);
    REQUIRE (f.openSong->isRecording());

    auto& strip = f.stripNamed ("Band Mix");
    CHECK (! strip.fader.isEnabled());
    CHECK (! strip.panControl.isEnabled());
    CHECK (! strip.muteButton.isEnabled());
    CHECK (! f.headerNamed ("Band Mix").muteButton.isEnabled());
    CHECK (! f.mixer().masterStrip.fader.isEnabled());
    CHECK (strip.meter.level() > 0.0f);
    CHECK (f.mixer().masterStrip.meter.level() > 0.0f);

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (strip.fader.isEnabled());
}

TEST_CASE ("the meters show what played and fall back once it stops", "[ui][mixer][meters]")
{
    MixerFixture f;
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (10);
    auto& bandMix = f.stripNamed ("Band Mix").meter;
    const auto playing = bandMix.level();
    CHECK (playing > 0.0f);
    CHECK_THAT (f.stripNamed ("Guitar").meter.level(), WithinAbs (0.0, 0.0));

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    f.runBlocks (1);
    CHECK (bandMix.level() < playing);
}

TEST_CASE ("double-clicking a Track fader, the Master fader or a pan sets 0 dB or C as one undo step", "[ui][mixer]")
{
    MixerFixture f;
    auto& strip = f.stripNamed ("Guitar");
    const auto grab = strip.fader.thumbCentre();
    drag (strip.fader, grab, grab.translated (0, 40));
    REQUIRE (f.trackNamed ("Guitar").volumeDb < -5.0);
    const auto dragged = f.song();

    doubleClickAt (strip.fader, strip.fader.thumbCentre());
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (strip.fader.db(), WithinAbs (0.0, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Volume");
    f.undo();
    CHECK (f.song() == dragged);
    f.undo();

    // Already at 0 dB: no undo step.
    const auto atUnity = f.song();
    const auto labelAtUnity = f.undoLabel();
    doubleClickAt (strip.fader, strip.fader.thumbCentre());
    CHECK (f.song() == atUnity);
    CHECK (f.undoLabel() == labelAtUnity);

    auto& master = f.mixer().masterStrip.fader;
    drag (master, master.thumbCentre(), master.thumbCentre().translated (0, -30));
    REQUIRE (f.song().master.volumeDb > 1.0);
    doubleClickAt (master, master.thumbCentre());
    CHECK_THAT (f.song().master.volumeDb, WithinAbs (0.0, 1.0e-9));
    CHECK (f.undoLabel() == "Master Volume");

    auto& pan = strip.panControl;
    const auto centre = pan.getLocalBounds().getCentre();
    drag (pan, centre, centre.translated (pan.getWidth() / 3, 0));
    REQUIRE (f.trackNamed ("Guitar").pan > 0.3);
    doubleClickAt (pan, centre);
    CHECK_THAT (f.trackNamed ("Guitar").pan, WithinAbs (0.0, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Pan");
    CHECK (pan.pan() == 0.0);
}

TEST_CASE ("after clicking a fader the arrow keys move it by 1 dB, 0.1 with shift, stopping at 0 dB", "[ui][mixer][keys]")
{
    MixerFixture f;
    f.useFixedClock();
    auto& strip = f.stripNamed ("Guitar");
    REQUIRE (f.trackNamed ("Guitar").volumeDb == 0.0);
    CHECK (! f.press (juce::KeyPress::upKey)); // nothing has focus: the window does nothing with it

    clickAt (strip.fader, strip.fader.thumbCentre());
    REQUIRE (f.mixer().mix.focusedControl() == MixerControl { f.trackNamed ("Guitar").id, MixerControlKind::volume });
    CHECK (strip.fader.isFocusShown());

    const auto expectDb = [&] (double db) {
        CHECK_THAT (strip.fader.db(), WithinAbs (db, 1.0e-9));
        CHECK (strip.valueLabel.getText() == juce::String (volumeText (db)));
    };
    CHECK (f.press (juce::KeyPress::upKey));
    expectDb (1.0);
    CHECK (f.press (juce::KeyPress::rightKey));
    expectDb (2.0);
    CHECK (f.press (juce::KeyPress::downKey));
    expectDb (1.0);
    CHECK (f.press (juce::KeyPress::leftKey));
    expectDb (0.0);
    for (int i = 0; i < 3; ++i)
        CHECK (f.press (juce::KeyPress::downKey, shift));
    expectDb (-0.3);
    f.window->tick(); // a poll shows the burst's value, not the Song's
    expectDb (-0.3);
    CHECK (f.press (juce::KeyPress::upKey));
    expectDb (0.0);
    CHECK (f.press (juce::KeyPress::upKey, shift));
    expectDb (0.1);
}

TEST_CASE ("after clicking a pan the left and right arrows move it by 5%, 1% with shift, stopping at C", "[ui][mixer][keys]")
{
    MixerFixture f;
    f.useFixedClock();
    auto& pan = f.stripNamed ("Vocals").panControl;
    clickAt (pan, pan.getLocalBounds().getCentre());
    REQUIRE (f.mixer().mix.focusedControl() == MixerControl { f.trackNamed ("Vocals").id, MixerControlKind::pan });
    CHECK (pan.isFocusShown());

    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK_THAT (pan.pan(), WithinAbs (0.1, 1.0e-9));
    CHECK (f.press (juce::KeyPress::leftKey, shift));
    CHECK_THAT (pan.pan(), WithinAbs (0.09, 1.0e-9));
    CHECK (f.press (juce::KeyPress::leftKey));
    CHECK_THAT (pan.pan(), WithinAbs (0.04, 1.0e-9));
    CHECK (f.press (juce::KeyPress::leftKey));
    CHECK_THAT (pan.pan(), WithinAbs (0.0, 1.0e-9));
    CHECK (f.press (juce::KeyPress::leftKey));
    CHECK_THAT (pan.pan(), WithinAbs (-0.05, 1.0e-9));
}

TEST_CASE ("while a fader or pan has focus the arrows do not move the playhead; after Esc or a click elsewhere they do", "[ui][mixer][keys]")
{
    MixerFixture f;
    f.useFixedClock();
    const auto nudge = (overdub::song::SampleCount) nudgeSeconds * f.song().sampleRate;
    auto& fader = f.stripNamed ("Guitar").fader;
    REQUIRE (f.openSong->position() == 0);

    clickAt (fader, fader.thumbCentre());
    f.mouseDownOn (fader); // the window's listener sees the click on the focused fader: it keeps focus
    REQUIRE (f.mixer().mix.focusedControl().has_value());
    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK (f.openSong->position() == 0);

    CHECK (f.press (juce::KeyPress::escapeKey));
    CHECK (! f.mixer().mix.focusedControl().has_value());
    CHECK (! fader.isFocusShown());
    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK (f.openSong->position() == nudge);

    auto& pan = f.stripNamed ("Guitar").panControl;
    clickAt (pan, pan.getLocalBounds().getCentre());
    f.mouseDownOn (pan);
    CHECK (f.press (juce::KeyPress::leftKey));
    CHECK (f.openSong->position() == nudge);

    f.mouseDownOn (f.window->songView()->editPage.timeline);
    CHECK (! f.mixer().mix.focusedControl().has_value());
    CHECK (f.press (juce::KeyPress::rightKey));
    CHECK (f.openSong->position() == 2 * nudge);
}

TEST_CASE ("other keys still reach the window while a fader has focus", "[ui][mixer][keys]")
{
    MixerFixture f;
    auto& fader = f.stripNamed ("Guitar").fader;
    clickAt (fader, fader.thumbCentre());
    REQUIRE (f.mixer().mix.focusedControl().has_value());
    CHECK (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.openSong->isPlaying());
}

TEST_CASE ("a burst of arrow presses is one undo step once 1 s passes with no press", "[ui][mixer][keys]")
{
    MixerFixture f;
    f.useFixedClock();
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.stripNamed ("Guitar").fader;
    clickAt (fader, fader.thumbCentre());

    const auto start = f.now;
    CHECK (f.press (juce::KeyPress::upKey));
    f.now = start + milliseconds (500);
    CHECK (f.press (juce::KeyPress::upKey));
    f.now = start + milliseconds (900);
    CHECK (f.press (juce::KeyPress::upKey));

    f.window->tick (start + milliseconds (1899)); // 999 ms after the last press
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    CHECK_THAT (fader.db(), WithinAbs (3.0, 1.0e-9));

    f.window->tick (start + milliseconds (1900));
    CHECK_THAT (f.trackNamed ("Guitar").volumeDb, WithinAbs (3.0, 1.0e-9));
    CHECK (f.undoLabel() == "Guitar Volume");

    f.undo();
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    CHECK_THAT (fader.db(), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("a burst of arrow presses is committed at once by focus loss, a mode change, another edit, undo or a Pass starting", "[ui][mixer][keys]")
{
    MixerFixture f;
    f.useFixedClock();
    const auto guitarId = f.trackNamed ("Guitar").id;
    auto& fader = f.stripNamed ("Guitar").fader;
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
        f.mouseDownOn (f.window->songView()->editPage.timeline);
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("grabbing another control")
    {
        startBurst();
        auto& pan = f.stripNamed ("Guitar").panControl;
        clickAt (pan, pan.getLocalBounds().getCentre());
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("a mode change")
    {
        startBurst();
        REQUIRE (pressKey (*f.window, '1'));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
        CHECK (! f.mixer().mix.focusedControl().has_value());
    }
    SECTION ("another edit keeps the burst as its own step before it")
    {
        startBurst();
        click (f.headerNamed ("Band Mix").muteButton);
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.trackNamed ("Band Mix").mute);
        CHECK (f.undoLabel() == "Mute Band Mix");
        f.undo();
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
    }
    SECTION ("a double-click on the same fader after the burst")
    {
        startBurst();
        doubleClickAt (fader, fader.thumbCentre());
        CHECK_THAT (guitarDb(), WithinAbs (0.0, 1.0e-9));
        f.undo();
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
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
        CHECK_THAT (fader.db(), WithinAbs (0.0, 1.0e-9));
    }
    SECTION ("redo commits the burst, which leaves nothing to redo")
    {
        click (f.headerNamed ("Band Mix").muteButton);
        f.undo();
        REQUIRE (f.openSong->package().canRedo());
        startBurst();
        REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
        CHECK (! f.trackNamed ("Band Mix").mute);
    }
    SECTION ("a Pass starting")
    {
        startBurst();
        REQUIRE (pressKey (*f.window, 'r'));
        CHECK_THAT (guitarDb(), WithinAbs (-2.0, 1.0e-9));
        CHECK (f.undoLabel() == "Guitar Volume");
        CHECK (! f.mixer().mix.focusedControl().has_value());
        REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey)); // a Pass ends before the window closes
        f.runBlocks (1);
    }
}

TEST_CASE ("while a Pass runs, double-click and the arrow keys change nothing in the Mixer", "[ui][mixer][keys][record]")
{
    MixerFixture f;
    f.useFixedClock();
    auto& strip = f.stripNamed ("Guitar");
    const auto grab = strip.fader.thumbCentre();
    drag (strip.fader, grab, grab.translated (0, 40));
    REQUIRE (f.mixer().mix.focusedControl().has_value());

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (20);
    REQUIRE (f.openSong->isRecording());
    const auto before = f.song();

    strip.fader.mouseDoubleClick (mouseEventAt (strip.fader, grab.toFloat(), {}));
    strip.panControl.mouseDoubleClick (mouseEventAt (strip.panControl, strip.panControl.getLocalBounds().getCentre().toFloat(), {}));
    f.press (juce::KeyPress::upKey);
    f.press (juce::KeyPress::rightKey);
    f.now += std::chrono::seconds (2);
    f.window->tick (f.now);
    CHECK (f.song().tracks == before.tracks);
    CHECK (f.song().master == before.master);
    CHECK (! f.mixer().mix.focusedControl().has_value());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("a horizontal fader behaves as a vertical one along its width: right is louder, with the same detent", "[ui][mixer]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    MixerFader fader (MixerFader::Orientation::horizontal);
    layOut (fader, 210, 20);
    REQUIRE (fader.travel() == 200);
    fader.setDb (-20.0);
    double released = 0.0;
    fader.onRelease = [&released] (double, double endDb) { released = endDb; };

    const auto grab = fader.thumbCentre();
    CHECK (grab == juce::Point<int> { 5 + 100, 10 }); // position 0.5 of 200 px, after half a thumb
    drag (fader, grab, grab.translated (54, 0));
    CHECK_THAT (released, WithinAbs (faderDragDb (-20.0, 54, 200), 1.0e-9));
    CHECK_THAT (released, WithinAbs (0.0, 1.0e-9)); // within the detent

    drag (fader, fader.thumbCentre(), fader.thumbCentre().translated (-25, 0));
    CHECK_THAT (released, WithinAbs (faderDragDb (0.0, -25, 200), 1.0e-9));
    CHECK (released < -5.0);

    bool reset = false;
    fader.onReset = [&reset] { reset = true; };
    doubleClickAt (fader, fader.thumbCentre());
    CHECK (reset);
}
