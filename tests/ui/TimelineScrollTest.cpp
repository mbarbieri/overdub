#include "ui/Timeline.h"

#include "core/LogEvents.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

// Vertical scrolling of the Edit Timeline's Track rows through the real window (issue #127, ADR
// 0005 UI amendment): the fixture's three Tracks fit; dropping two audio files makes five, which
// overflow. Then wheel movements, clicks on scrolled lanes, and what moves the view on its own.
using overdub::core::ScopedLogCapture;
using namespace overdub::ui;
using namespace overdub::ui::testing;
namespace song = overdub::song;

namespace
{
struct ScrollFixture : MainWindowFixture
{
    ScrollFixture()
    {
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (isShown (timeline()));
    }

    Timeline& timeline() { return window->editPage.timeline; }

    // Drops these new audio files on the Timeline: one new Track each, at the end.
    void dropFiles (const std::vector<std::string>& names)
    {
        juce::StringArray paths;
        for (const auto& name : names)
            paths.add (fixture->writeSineFile (name, 1.0, 110.0).getFullPathName());
        REQUIRE (timeline().isInterestedInFileDrag (paths));
        timeline().filesDropped (paths, 10, 10);
    }

    void wheel (float deltaX, float deltaY, juce::ModifierKeys modifiers = {})
    {
        const auto at = timeline().tracksViewportBounds().getCentre().toFloat();
        juce::MouseWheelDetails details {};
        details.deltaX = deltaX;
        details.deltaY = deltaY;
        timeline().mouseWheelMove (mouseEventAt (timeline(), at, modifiers), details);
    }

    // Where this Track's only Clip is drawn (empty when scrolled out of view).
    juce::Rectangle<int> clipOf (const std::string& trackName)
    {
        const auto& clips = trackNamed (trackName).clips;
        REQUIRE (clips.size() == 1);
        return timeline().clipBounds (clips[0].id);
    }

    // The Track header's bounds in the Timeline's coordinates.
    juce::Rectangle<int> headerBounds (const std::string& trackName)
    {
        auto& header = editHeaderNamed (trackName);
        return timeline().getLocalArea (&header, header.getLocalBounds());
    }

    // Scrolls the Track rows back to the top.
    void scrollToTop()
    {
        timeline().setVerticalScroll (0);
        REQUIRE (timeline().verticalScroll() == 0);
    }
};

bool viewportShows (Timeline& timeline, juce::Rectangle<int> bounds)
{
    return ! bounds.isEmpty() && timeline.tracksViewportBounds().contains (bounds);
}
} // namespace

TEST_CASE ("Tracks that fit show no vertical scroll bar; Tracks that overflow show one", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    CHECK_FALSE (isShown (f.timeline().verticalScrollBar));
    CHECK (f.timeline().verticalScroll() == 0);

    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    REQUIRE (f.song().tracks.size() == 5);
    CHECK (isShown (f.timeline().verticalScrollBar));
    CHECK (f.timeline().verticalScrollBar.getX() >= f.timeline().rulerBounds().getRight());
}

TEST_CASE ("Tracks added below the visible rows by a Drop or an Import are scrolled into view", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    ScopedLogCapture capture;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });

    CHECK (f.timeline().verticalScroll() > 0);
    CHECK (viewportShows (f.timeline(), f.clipOf ("Drums")));
    CHECK (viewportShows (f.timeline(), f.clipOf ("Bass")));
    const auto revealed = overdub::core::testing::firstEvent (capture, "ui.timeline_tracks_revealed");
    CHECK (revealed.at ("first_track_id") == f.trackNamed ("Drums").id);
    CHECK (revealed.at ("new_tracks") == 2);

    f.scrollToTop();
    REQUIRE (f.openSong->importFiles ({ f.fixture->writeSineFile ("Keys.wav", 1.0, 220.0) }).front().has_value());
    f.runBlocks (1);
    CHECK (viewportShows (f.timeline(), f.clipOf ("Keys")));
}

TEST_CASE ("a vertical wheel scrolls Track headers and lanes together and leaves time, the ruler and the Camera lane alone", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.scrollToTop();
    const auto ruler = f.timeline().rulerBounds();
    const auto camera = f.timeline().cameraLaneBounds();
    const auto firstSample = f.timeline().view().firstSample;
    const auto header = f.headerBounds ("Guitar");
    const auto clip = f.clipOf ("Drums");

    f.wheel (0.0f, -0.1f); // 40 pixels down the list

    CHECK (f.timeline().verticalScroll() == 40);
    CHECK (f.timeline().view().firstSample == firstSample);
    CHECK (f.timeline().rulerBounds() == ruler);
    CHECK (f.timeline().cameraLaneBounds() == camera);
    CHECK (f.headerBounds ("Guitar") == header.translated (0, -40));
    CHECK (f.clipOf ("Drums").getY() == clip.getY() - 40);
    CHECK ((int) f.timeline().verticalScrollBar.getCurrentRangeStart() == 40);
}

TEST_CASE ("a horizontal swipe, or shift with the wheel, scrolls time and leaves the Tracks' scroll alone", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.scrollToTop();
    const auto samplesPerPixel = f.timeline().view().samplesPerPixel;

    f.wheel (-0.1f, 0.0f);
    CHECK (f.timeline().view().firstSample == 40 * samplesPerPixel);
    CHECK (f.timeline().verticalScroll() == 0);

    f.wheel (0.0f, -0.1f, juce::ModifierKeys::shiftModifier);
    CHECK (f.timeline().view().firstSample == 80 * samplesPerPixel);
    CHECK (f.timeline().verticalScroll() == 0);
}

TEST_CASE ("the vertical scroll bar scrolls the Track rows", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.scrollToTop();

    f.timeline().verticalScrollBar.setCurrentRangeStart (30.0, juce::sendNotificationSync);
    CHECK (f.timeline().verticalScroll() == 30);
}

TEST_CASE ("dragging a Clip on a scrolled lane moves the Clip drawn under the pointer", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    REQUIRE (f.timeline().verticalScroll() > 0);
    const auto bounds = f.clipOf ("Bass");
    REQUIRE (viewportShows (f.timeline(), bounds));
    const auto drumsBefore = f.trackNamed ("Drums").clips[0];

    drag (f.timeline(), bounds.getCentre(), bounds.getCentre().translated (20, 0));

    CHECK (f.trackNamed ("Bass").clips[0].timelineStart == 20 * f.timeline().view().samplesPerPixel);
    CHECK (f.trackNamed ("Drums").clips[0] == drumsBefore);
    CHECK (f.timeline().selectedClip() == f.trackNamed ("Bass").clips[0].id);
}

TEST_CASE ("trimming a Clip and dragging its dB tag on a scrolled lane edit the Clip drawn under the pointer", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    REQUIRE (f.timeline().verticalScroll() > 0);
    const auto samplesPerPixel = f.timeline().view().samplesPerPixel;
    const auto& bassClipId = f.trackNamed ("Bass").clips[0].id;
    const auto drumsBefore = f.trackNamed ("Drums").clips[0];

    const auto tag = f.timeline().handleBounds (bassClipId, ClipPart::gainTag);
    REQUIRE (viewportShows (f.timeline(), tag));
    drag (f.timeline(), tag.getCentre(), tag.getCentre().translated (0, -10));
    CHECK (std::abs (f.trackNamed ("Bass").clips[0].gainDb - 1.0) < 1e-9);
    CHECK (f.openSong->package().undoLabel() == "Clip Gain");

    const auto bounds = f.clipOf ("Bass");
    const juce::Point<int> startEdge { bounds.getX() + 1, bounds.getCentreY() };
    drag (f.timeline(), startEdge, startEdge.translated (10, 0));
    CHECK (f.trackNamed ("Bass").clips[0].timelineStart == 10 * samplesPerPixel);
    CHECK (f.openSong->package().undoLabel() == "Trim Clip");
    CHECK (f.trackNamed ("Drums").clips[0] == drumsBefore);
}

TEST_CASE ("a Track scrolled up under the Camera lane is cut off there, and a click on the Camera lane does not reach it", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.timeline().setVerticalScroll (60);
    REQUIRE (f.timeline().verticalScroll() == 60);
    const auto viewportTop = f.timeline().tracksViewportBounds().getY();

    // Band Mix's row starts 60 pixels above the viewport: its Clip and header are cut at its top.
    const auto bandMix = f.clipOf ("Band Mix");
    CHECK (bandMix.getY() == viewportTop);
    auto& headerColumn = *f.editHeaderNamed ("Band Mix").getParentComponent();
    CHECK (f.timeline().getLocalArea (&headerColumn, headerColumn.getLocalBounds()).getY() == viewportTop);

    // Where Band Mix's Clip would be drawn without the cut, inside the Camera lane.
    const auto songBefore = f.song();
    f.timeline().selectClip (std::nullopt);
    clickAt (f.timeline(), { bandMix.getCentreX(), f.timeline().cameraLaneBounds().getCentreY() });
    CHECK_FALSE (f.timeline().selectedClip().has_value());
    CHECK (f.song() == songBefore);
}

TEST_CASE ("unfolding Take Lanes and selecting a Clip leave the vertical scroll where it was", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    // A Pass on the Armed Guitar Track, so Guitar has a Take Lane.
    REQUIRE (pressKey (*f.window, '1'));
    f.openSong->seek (0);
    f.runBlocks (0);
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (200);
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (pressKey (*f.window, '2'));
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.timeline().setVerticalScroll (30);
    REQUIRE (f.timeline().verticalScroll() == 30);

    const auto& guitar = f.trackNamed ("Guitar");
    f.timeline().setTakeLanesUnfolded (guitar.id, true);
    REQUIRE (! f.timeline().takeLanesShown (guitar.id).empty());
    CHECK (f.timeline().verticalScroll() == 30);

    const auto drums = f.clipOf ("Drums");
    REQUIRE (viewportShows (f.timeline(), drums));
    clickAt (f.timeline(), drums.getCentre());
    REQUIRE (f.timeline().selectedClip() == f.trackNamed ("Drums").clips[0].id);
    CHECK (f.timeline().verticalScroll() == 30);

    // Folding them again at the bottom clamps the scroll: the last Track ends at the rows' bottom.
    f.timeline().setVerticalScroll (10000);
    const auto unfoldedScroll = f.timeline().verticalScroll();
    f.timeline().setTakeLanesUnfolded (guitar.id, false);
    CHECK (f.timeline().verticalScroll() < unfoldedScroll);
    CHECK (f.headerBounds ("Bass").getBottom() == f.timeline().tracksViewportBounds().getBottom());
}

TEST_CASE ("when the Timeline grows, the vertical scroll is clamped to what the Track rows still need", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.timeline().setVerticalScroll (10000);
    REQUIRE (f.timeline().verticalScroll() > 0);

    click (f.window->editPage.toolbar.mixerButton); // hides the Mixer: the Timeline gets its room
    CHECK (f.timeline().verticalScroll() == 0);
    CHECK_FALSE (isShown (f.timeline().verticalScrollBar));
}

TEST_CASE ("when the Track rows shrink, the vertical scroll is clamped so no space is left under the last Track", "[ui][edit][vertical_scroll]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav" });
    f.timeline().setVerticalScroll (10000);
    const auto viewport = f.timeline().tracksViewportBounds();
    CHECK (f.headerBounds ("Bass").getBottom() == viewport.getBottom());

    // Deleting two Tracks makes the rest fit: back to the top, no vertical scroll bar.
    for (const auto* name : { "Drums", "Bass" })
    {
        click (f.editHeaderNamed (name).deleteButton);
        f.answerDeleteQuestion (true);
    }
    REQUIRE (f.song().tracks.size() == 3);
    CHECK (f.timeline().verticalScroll() == 0);
    CHECK_FALSE (isShown (f.timeline().verticalScrollBar));

    // Nothing to scroll: the wheel leaves the rows where they are.
    f.wheel (0.0f, -0.1f);
    CHECK (f.timeline().verticalScroll() == 0);
}

TEST_CASE ("snapshot: the Edit page with Tracks scrolled under the ruler and Camera lane", "[ui][edit][vertical_scroll][snapshot]")
{
    ScrollFixture f;
    f.dropFiles ({ "Drums.wav", "Bass.wav", "Keys.wav" });
    f.timeline().setVerticalScroll (50);
    writeSnapshot (*f.window, "127-vertical-scroll", "edit-scrolled");
}
