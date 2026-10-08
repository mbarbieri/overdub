#include "ui/Mixer.h"

#include "ui/MainWindowFixture.h"
#include "ui/MixerGeometry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

    Mixer& mixer() { return window->editPage.mixer; }

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
        for (auto& header : window->editPage.timeline.headers)
            if (header->trackId() == trackId)
                return *header;
        FAIL ("no header for Track " << name);
        return *window->editPage.timeline.headers.front();
    }

    std::string undoLabel() const { return openSong->package().undoLabel(); }

    void undo() { REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier)); }
};

// The fader thumb's centre, in the fader's coordinates.
juce::Point<int> thumbCentre (const MixerFader& fader)
{
    const auto y = MixerFader::thumbHeight / 2 + (int) std::lround ((1.0 - dbToFaderPosition (fader.db())) * fader.travel());
    return { fader.getWidth() / 2, y };
}
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
    const auto grab = thumbCentre (fader);
    const juce::ModifierKeys leftButton (juce::ModifierKeys::leftButtonModifier);

    // Down, two moves (10 px up, then 30 px up from the grab): nothing is committed yet.
    fader.mouseDown (mouseEventAt (fader, grab.toFloat(), leftButton));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (0, -10).toFloat(), leftButton, grab.toFloat()));
    fader.mouseDrag (mouseEventAt (fader, grab.translated (0, -30).toFloat(), leftButton, grab.toFloat()));
    f.window->tick(); // a poll during the drag keeps the dragged value shown
    CHECK (f.song() == before);
    CHECK (f.undoLabel() == labelBefore);
    const auto expectedDb = faderDragDb (0.0, -30, fader.travel());
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
    const auto grab = thumbCentre (fader);
    drag (fader, grab, grab.translated (0, 40));

    CHECK_THAT (f.song().master.volumeDb, WithinAbs (faderDragDb (0.0, 40, fader.travel()), 1.0e-9));
    CHECK (f.song().master.volumeDb < -5.0);
    CHECK (f.undoLabel() == "Master Volume");
}

TEST_CASE ("dragging the pan control sets the Track's pan as one Pan step", "[ui][mixer]")
{
    MixerFixture f;
    auto& pan = f.stripNamed ("Vocals").panControl;
    const auto centre = pan.getLocalBounds().getCentre();
    drag (pan, centre, centre.translated (-pan.getWidth() / 4, 0));

    CHECK_THAT (f.trackNamed ("Vocals").pan, WithinAbs (panDrag (0.0, -pan.getWidth() / 4, pan.getWidth()), 1.0e-9));
    CHECK (f.trackNamed ("Vocals").pan < -0.4);
    CHECK (f.undoLabel() == "Vocals Pan");
}

TEST_CASE ("a fader or pan drag that ends where it started changes nothing", "[ui][mixer]")
{
    MixerFixture f;
    const auto before = f.song();
    const auto labelBefore = f.undoLabel();
    auto& fader = f.stripNamed ("Guitar").fader;
    const auto grab = thumbCentre (fader);
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
