#include "ui/EditPage.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

// Edit mode's timeline through the real window (issue #28, ADR 0005 UI amendment): synthesized
// drags and keys, then the Song, the undo step's label, and what undo restores.
using overdub::core::ScopedLogCapture;
using overdub::song::SampleCount;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
constexpr SampleCount bandMixLength = 96000; // the fixture's 2 s Backing Track at 48 kHz

struct EditFixture : MainWindowFixture
{
    EditFixture()
    {
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (isShown (window->editPage.timeline));
    }

    Timeline& timeline() { return window->editPage.timeline; }

    const overdub::song::Clip& bandMixClip (std::size_t index = 0)
    {
        const auto& clips = trackNamed ("Band Mix").clips;
        REQUIRE (index < clips.size());
        return clips[index];
    }

    juce::Rectangle<int> bandMixBounds()
    {
        const auto bounds = timeline().clipBounds (bandMixClip().id);
        REQUIRE (! bounds.isEmpty());
        return bounds;
    }

    SampleCount samplesPerPixel() { return timeline().view().samplesPerPixel; }

    void undoRestores (const overdub::song::Song& before)
    {
        REQUIRE (pressKey (*window, 'z', juce::ModifierKeys::commandModifier));
        CHECK (song() == before);
    }
};

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("dragging a Clip's body moves it by the dragged pixels in samples, as one Move Clip step", "[ui][edit]")
{
    EditFixture f;
    const auto before = f.song();
    const auto bounds = f.bandMixBounds();

    drag (f.timeline(), bounds.getCentre(), bounds.getCentre().translated (50, 0));

    CHECK (f.bandMixClip().timelineStart == 50 * f.samplesPerPixel());
    CHECK (f.bandMixClip().sourceOffset == 0);
    CHECK (f.bandMixClip().length == bandMixLength);
    CHECK (f.openSong->package().undoLabel() == "Move Clip");
    CHECK (f.window->topBar.undoButton.getButtonText() == "Undo Move Clip");
    f.undoRestores (before);
}

TEST_CASE ("dragging a Clip's start edge trims its start, as one Trim Clip step", "[ui][edit]")
{
    EditFixture f;
    const auto before = f.song();
    const auto bounds = f.bandMixBounds();
    const juce::Point<int> startEdge { bounds.getX() + 1, bounds.getCentreY() };

    drag (f.timeline(), startEdge, startEdge.translated (20, 0));

    const auto trimmedBy = 20 * f.samplesPerPixel();
    CHECK (f.bandMixClip().timelineStart == trimmedBy);
    CHECK (f.bandMixClip().sourceOffset == trimmedBy);
    CHECK (f.bandMixClip().length == bandMixLength - trimmedBy);
    CHECK (f.openSong->package().undoLabel() == "Trim Clip");
    f.undoRestores (before);
}

TEST_CASE ("dragging a Clip's end edge trims its end, and not past its source", "[ui][edit]")
{
    EditFixture f;
    const auto before = f.song();
    const auto bounds = f.bandMixBounds();
    const juce::Point<int> endEdge { bounds.getRight() - 2, bounds.getCentreY() };

    drag (f.timeline(), endEdge, endEdge.translated (-40, 0));
    CHECK (f.bandMixClip().length == bandMixLength - 40 * f.samplesPerPixel());
    CHECK (f.bandMixClip().timelineStart == 0);
    CHECK (f.openSong->package().undoLabel() == "Trim Clip");
    f.undoRestores (before);

    // Outwards: the Backing Track has no samples past its end.
    drag (f.timeline(), endEdge, endEdge.translated (100, 0));
    CHECK (f.song() == before);
}

TEST_CASE ("a click on a Clip selects it without changing the Song", "[ui][edit]")
{
    EditFixture f;
    const auto before = f.song();
    clickAt (f.timeline(), f.bandMixBounds().getCentre());

    CHECK (f.timeline().selectedClip() == f.bandMixClip().id);
    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == "Add Tracks");
    CHECK (f.window->editPage.toolbar.deleteButton.isEnabled());
    CHECK (f.window->editPage.toolbar.details.getText().startsWith ("Band Mix.wav on Band Mix"));

    // Empty lane space clears the selection.
    clickAt (f.timeline(), { f.bandMixBounds().getRight() + 100, f.bandMixBounds().getCentreY() });
    CHECK (! f.timeline().selectedClip().has_value());
    CHECK (! f.window->editPage.toolbar.deleteButton.isEnabled());
}

TEST_CASE ("S splits the selected Clip at the playhead, as one Split Clip step", "[ui][edit]")
{
    EditFixture f;
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    f.openSong->seek (30000);
    f.runBlocks (0);
    const auto before = f.song();
    CHECK (f.window->editPage.toolbar.splitButton.isEnabled());

    REQUIRE (pressKey (*f.window, 's'));

    const auto& clips = f.trackNamed ("Band Mix").clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[0].timelineStart == 0);
    CHECK (clips[0].length == 30000);
    CHECK (clips[1].timelineStart == 30000);
    CHECK (clips[1].sourceOffset == 30000);
    CHECK (clips[1].length == bandMixLength - 30000);
    CHECK (f.timeline().selectedClip() == clips[1].id);
    CHECK (f.openSong->package().undoLabel() == "Split Clip");
    f.undoRestores (before);
}

TEST_CASE ("backspace deletes the selected Clip, as one Delete Clip step", "[ui][edit]")
{
    EditFixture f;
    const auto before = f.song();
    clickAt (f.timeline(), f.bandMixBounds().getCentre());

    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));

    CHECK (f.trackNamed ("Band Mix").clips.empty());
    CHECK (f.song().backingTracks == before.backingTracks);
    CHECK (! f.timeline().selectedClip().has_value());
    CHECK (f.openSong->package().undoLabel() == "Delete Clip");
    f.undoRestores (before);
}

TEST_CASE ("the Split and Delete buttons act like S and backspace", "[ui][edit]")
{
    EditFixture f;
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    f.openSong->seek (40000);
    f.runBlocks (0);

    click (f.window->editPage.toolbar.splitButton);
    CHECK (f.trackNamed ("Band Mix").clips.size() == 2);
    CHECK (f.openSong->package().undoLabel() == "Split Clip");

    click (f.window->editPage.toolbar.deleteButton);
    CHECK (f.trackNamed ("Band Mix").clips.size() == 1);
    CHECK (f.bandMixClip().length == 40000);
    CHECK (f.openSong->package().undoLabel() == "Delete Clip");
}

TEST_CASE ("S with the playhead outside the selected Clip shows a Notice and changes nothing", "[ui][edit]")
{
    EditFixture f;
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    f.openSong->seek (bandMixLength + 1000);
    f.runBlocks (0);
    const auto before = f.song();
    CHECK (! f.window->editPage.toolbar.splitButton.isEnabled());

    ScopedLogCapture capture;
    REQUIRE (pressKey (*f.window, 's'));

    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == "Add Tracks");
    const auto shown = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (shown.size() == 1);
    CHECK (shown[0].at ("code") == "song.split_outside_clip");
    CHECK (isShown (f.window->noticeLine));
}

TEST_CASE ("S and backspace do nothing outside Edit mode or with no Clip selected", "[ui][edit]")
{
    EditFixture f;
    f.openSong->seek (30000);
    f.runBlocks (0);
    const auto before = f.song();

    REQUIRE (pressKey (*f.window, 's'));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (f.song() == before);

    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    REQUIRE (pressKey (*f.window, '1'));
    REQUIRE (pressKey (*f.window, 's'));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (f.song() == before);
}

TEST_CASE ("Clips cannot be edited while a Pass records", "[ui][edit][record]")
{
    EditFixture f;
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (10);
    REQUIRE (f.openSong->isRecording());
    const auto before = f.song();

    const auto bounds = f.bandMixBounds();
    drag (f.timeline(), bounds.getCentre(), bounds.getCentre().translated (50, 0));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::backspaceKey)));
    CHECK (f.song() == before);
    CHECK (! f.window->editPage.toolbar.deleteButton.isEnabled());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
}

TEST_CASE ("clicking the ruler moves the playhead there", "[ui][edit]")
{
    EditFixture f;
    const auto ruler = f.timeline().rulerBounds();
    clickAt (f.timeline(), { ruler.getX() + 100, ruler.getCentreY() });
    CHECK (f.openSong->position() == 100 * f.samplesPerPixel());
}

TEST_CASE ("the left and right arrows move the playhead back and forward by 5 seconds, not before the start", "[ui][edit]")
{
    EditFixture f;
    const SampleCount nudge = 5 * f.song().sampleRate;
    f.openSong->seek (3 * nudge);

    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK (f.openSong->position() == 4 * nudge);

    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::leftKey)));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (f.openSong->position() == 2 * nudge);

    f.openSong->seek (nudge / 2);
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (f.openSong->position() == 0);
}

TEST_CASE ("the arrows do not move the playhead outside Edit mode", "[ui][edit]")
{
    EditFixture f;
    REQUIRE (pressKey (*f.window, '1'));
    f.openSong->seek (100000);

    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::rightKey)));
    REQUIRE (pressKey (*f.window, juce::KeyPress (juce::KeyPress::leftKey)));
    CHECK (f.openSong->position() == 100000);
}

TEST_CASE ("the zoom buttons halve and double the samples per pixel, and Clips redraw to match", "[ui][edit]")
{
    EditFixture f;
    const auto start = f.samplesPerPixel();
    const auto width = f.bandMixBounds().getWidth();

    click (f.window->editPage.toolbar.zoomInButton);
    CHECK (f.samplesPerPixel() == start / 2);
    CHECK (f.bandMixBounds().getWidth() == (int) (bandMixLength / f.samplesPerPixel()));

    click (f.window->editPage.toolbar.zoomOutButton);
    click (f.window->editPage.toolbar.zoomOutButton);
    CHECK (f.samplesPerPixel() == start * 2);
    CHECK (f.bandMixBounds().getWidth() < width);

    // Drags still move by whole pixels' worth of samples at the new zoom.
    const auto bounds = f.bandMixBounds();
    drag (f.timeline(), bounds.getCentre(), bounds.getCentre().translated (10, 0));
    CHECK (f.bandMixClip().timelineStart == 10 * start * 2);
}

TEST_CASE ("scrolling shows the Song from a later sample", "[ui][edit]")
{
    EditFixture f;
    f.timeline().setView (48000, f.samplesPerPixel());
    CHECK (f.timeline().view().firstSample == 48000);
    CHECK (f.bandMixBounds().getX() == f.timeline().rulerBounds().getX());

    f.timeline().scrollBar.setCurrentRangeStart (0.0, juce::sendNotificationSync);
    CHECK (f.timeline().view().firstSample == 0);
}

TEST_CASE ("the timeline shows the Camera lane and one header per Track whose arm button arms it", "[ui][edit]")
{
    EditFixture f;
    auto& timeline = f.timeline();
    CHECK (isShown (timeline.cameraHeader));
    CHECK (! timeline.cameraLaneBounds().isEmpty());
    REQUIRE (timeline.headers.size() == 3);
    CHECK (timeline.headers[1]->nameLabel.getText() == "Guitar");
    CHECK (timeline.headers[1]->armButton.getToggleState());

    click (timeline.headers[1]->armButton);
    CHECK (! f.inputOf ("Guitar").armed);
    CHECK (! timeline.headers[1]->armButton.getToggleState());
}

TEST_CASE ("the Mixer region hides and shows with its button and resizes with its divider", "[ui][edit][mixer]")
{
    EditFixture f;
    auto& page = f.window->editPage;
    REQUIRE (isShown (page.mixer));
    CHECK (page.mixer.getHeight() == defaultMixerHeight);
    const auto timelineHeight = page.timeline.getHeight();
    const auto stripHeight = page.mixer.masterStrip.getHeight();
    const auto faderTravel = page.mixer.strips[0]->fader.travel();

    // Drag the divider up by 50: the region grows by 50 and the timeline shrinks by 50; the strips
    // and their faders grow with it.
    const juce::Point<int> grab { page.divider.getWidth() / 2, 2 };
    drag (page.divider, grab, grab.translated (0, -50));
    CHECK (page.mixer.getHeight() == defaultMixerHeight + 50);
    CHECK (page.timeline.getHeight() == timelineHeight - 50);
    CHECK (page.mixer.getBottom() == page.getHeight());
    CHECK (page.mixer.masterStrip.getHeight() == stripHeight + 50);
    CHECK (page.mixer.strips[0]->fader.travel() == faderTravel + 50);

    click (page.toolbar.mixerButton);
    CHECK (! isShown (page.mixer));
    CHECK (! isShown (page.divider));
    CHECK (page.timeline.getBottom() == page.getHeight());

    click (page.toolbar.mixerButton);
    CHECK (isShown (page.mixer));
    CHECK (page.mixer.getHeight() == defaultMixerHeight + 50);
}

TEST_CASE ("the Clip toolbar names the Clip, its Track, start and length", "[ui][edit]")
{
    auto song = overdub::song::makeEmptySong (48000);
    song.passes.push_back ({ .id = "p1" });
    song.passes.push_back ({ .id = "p2" });
    song.takes.push_back ({ .id = "take", .passId = "p2", .lengthSamples = 200000 });
    overdub::song::Track track;
    track.name = "Guitar";
    track.clips.push_back ({ .id = "c", .source = { overdub::song::ClipSourceKind::take, "take" }, .timelineStart = 60000, .length = 144000 });
    song.tracks.push_back (track);

    const auto selected = clipToolbarStateFor (song, std::string ("c"), 100000, true);
    CHECK (selected.hasSelection);
    CHECK (selected.details == "Pass 2 on Guitar \xC2\xB7 start 1.250 s \xC2\xB7 length 3.000 s");
    CHECK (selected.canSplit);
    CHECK (selected.canDelete);

    CHECK (! clipToolbarStateFor (song, std::string ("c"), 60000, true).canSplit); // on the edge
    CHECK (! clipToolbarStateFor (song, std::string ("c"), 100000, false).canDelete);
    CHECK (! clipToolbarStateFor (song, std::nullopt, 100000, true).hasSelection);
    CHECK (! clipToolbarStateFor (song, std::string ("gone"), 100000, true).hasSelection);
    CHECK (secondsText (-24, 48000) == "-0.000 s");
    CHECK (secondsText (48000 * 83 + 24, 48000) == "83.000 s");
}

TEST_CASE ("dragging a Clip's fade-in handle right sets its fade-in, as one Fade In step", "[ui][edit][clip_gain]")
{
    EditFixture f;
    const auto before = f.song();
    const auto handle = f.timeline().handleBounds (f.bandMixClip().id, ClipPart::fadeInHandle);
    REQUIRE (! handle.isEmpty());

    drag (f.timeline(), handle.getCentre(), handle.getCentre().translated (30, 0));

    CHECK (f.bandMixClip().fadeInLength == 30 * f.samplesPerPixel());
    CHECK (f.bandMixClip().fadeOutLength == 0);
    CHECK (f.bandMixClip().timelineStart == 0);
    CHECK (f.bandMixClip().length == bandMixLength);
    CHECK (f.openSong->package().undoLabel() == "Fade In");
    CHECK (f.window->topBar.undoButton.getButtonText() == "Undo Fade In");
    f.undoRestores (before);
}

TEST_CASE ("dragging a Clip's fade-out handle left sets its fade-out, as one Fade Out step", "[ui][edit][clip_gain]")
{
    EditFixture f;
    const auto before = f.song();
    const auto handle = f.timeline().handleBounds (f.bandMixClip().id, ClipPart::fadeOutHandle);
    REQUIRE (! handle.isEmpty());

    drag (f.timeline(), handle.getCentre(), handle.getCentre().translated (-40, 0));

    CHECK (f.bandMixClip().fadeOutLength == 40 * f.samplesPerPixel());
    CHECK (f.bandMixClip().fadeInLength == 0);
    CHECK (f.bandMixClip().length == bandMixLength);
    CHECK (f.openSong->package().undoLabel() == "Fade Out");
    f.undoRestores (before);

    // Past the other end: the fade-out takes the whole Clip and no more.
    drag (f.timeline(), handle.getCentre(), handle.getCentre().translated (-1000, 0));
    CHECK (f.bandMixClip().fadeOutLength == bandMixLength);
}

TEST_CASE ("dragging a Clip's dB tag up raises its gain, as one Clip Gain step", "[ui][edit][clip_gain]")
{
    EditFixture f;
    const auto before = f.song();
    const auto tag = f.timeline().handleBounds (f.bandMixClip().id, ClipPart::gainTag);
    REQUIRE (! tag.isEmpty());

    drag (f.timeline(), tag.getCentre(), tag.getCentre().translated (0, -25));

    CHECK (std::abs (f.bandMixClip().gainDb - 2.5) < 1e-9);
    CHECK (f.bandMixClip().timelineStart == 0);
    CHECK (f.openSong->package().undoLabel() == "Clip Gain");
    CHECK (f.timeline().selectedClip() == f.bandMixClip().id);
    f.undoRestores (before);
}

TEST_CASE ("the Clip toolbar's fade and gain fields edit the selected Clip, one labelled step each", "[ui][edit][clip_gain]")
{
    EditFixture f;
    auto& toolbar = f.window->editPage.toolbar;
    CHECK (! toolbar.fadeInField.isEnabled());
    CHECK (! toolbar.gainField.isEnabled());

    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    REQUIRE (toolbar.fadeInField.isEnabled());
    CHECK (toolbar.fadeInField.getText() == "0.000 s");
    CHECK (toolbar.gainField.getText() == "0.0 dB");
    const auto before = f.song();

    toolbar.fadeInField.setText ("0.5", juce::sendNotificationSync);
    CHECK (f.bandMixClip().fadeInLength == 24000);
    CHECK (f.openSong->package().undoLabel() == "Fade In");
    CHECK (toolbar.fadeInField.getText() == "0.500 s");

    toolbar.fadeOutField.setText ("0.25 s", juce::sendNotificationSync);
    CHECK (f.bandMixClip().fadeOutLength == 12000);
    CHECK (f.openSong->package().undoLabel() == "Fade Out");

    toolbar.gainField.setText ("-6", juce::sendNotificationSync);
    CHECK (std::abs (f.bandMixClip().gainDb + 6.0) < 1e-9);
    CHECK (f.openSong->package().undoLabel() == "Clip Gain");
    CHECK (toolbar.gainField.getText() == "-6.0 dB");

    // Clamped: the fade-in can take only what the fade-out leaves.
    toolbar.fadeInField.setText ("10", juce::sendNotificationSync);
    CHECK (f.bandMixClip().fadeInLength == bandMixLength - 12000);
    CHECK (toolbar.fadeInField.getText() == "1.750 s");

    // Not a number: nothing changes and the field shows the Clip's value again.
    const auto beforeNonsense = f.song();
    toolbar.gainField.setText ("loud", juce::sendNotificationSync);
    CHECK (f.song() == beforeNonsense);
    CHECK (toolbar.gainField.getText() == "-6.0 dB");

    // Four steps, each undone on its own.
    for (int step = 0; step < 4; ++step)
        REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song() == before);
}

TEST_CASE ("Clip gain and fades cannot be edited while a Pass records", "[ui][edit][clip_gain][record]")
{
    EditFixture f;
    clickAt (f.timeline(), f.bandMixBounds().getCentre());
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (10);
    REQUIRE (f.openSong->isRecording());
    const auto before = f.song();

    const auto tag = f.timeline().handleBounds (f.bandMixClip().id, ClipPart::gainTag);
    drag (f.timeline(), tag.getCentre(), tag.getCentre().translated (0, -25));
    CHECK (! f.window->editPage.toolbar.gainField.isEnabled());
    CHECK (f.song() == before);

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
}

TEST_CASE ("the toolbar parses seconds and decibels, and shows gain with its sign", "[ui][edit][clip_gain]")
{
    CHECK (parseSeconds ("0.5", 48000) == 24000);
    CHECK (parseSeconds (" 1.25 s ", 48000) == 60000);
    CHECK (parseSeconds ("0", 48000) == 0);
    CHECK (! parseSeconds ("", 48000).has_value());
    CHECK (! parseSeconds ("abc", 48000).has_value());
    CHECK (! parseSeconds ("1.5x", 48000).has_value());
    CHECK (! parseSeconds ("-1", 48000).has_value());

    const auto parsesTo = [] (const char* text, double expected) {
        const auto parsed = parseDecibels (text);
        return parsed.has_value() && std::abs (*parsed - expected) < 1e-12;
    };
    CHECK (parsesTo ("-6", -6.0));
    CHECK (parsesTo ("+3.5 dB", 3.5));
    CHECK (parsesTo ("2db", 2.0));
    CHECK (! parseDecibels ("dB").has_value());
    CHECK (! parseDecibels ("nan").has_value());

    CHECK (decibelsText (0.0) == "0.0 dB");
    CHECK (decibelsText (2.5) == "+2.5 dB");
    CHECK (decibelsText (-12.0) == "-12.0 dB");
}
