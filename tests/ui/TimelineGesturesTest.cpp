#include "ui/Timeline.h"

#include "core/LogEvents.h"
#include "ui/MainWindowFixture.h"
#include "ui/SongEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// The Timeline runs one gesture at a time (issue #148): a Clip drag, a ruler drag, a grip drag or a
// file drag hovering. Synthesized mouse events, file drags and Esc through the real window, then
// what the Timeline shows, the Song and its undo step.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
const juce::ModifierKeys leftButton { juce::ModifierKeys::leftButtonModifier };

struct GestureFixture : MainWindowFixture
{
    GestureFixture()
    {
        REQUIRE (pressKey (*window, '2'));
        REQUIRE (isShown (timeline()));
    }

    Timeline& timeline() { return window->songView()->editPage.timeline; }

    const song::Id& bandMixClipId() { return trackNamed ("Band Mix").clips.front().id; }

    juce::Rectangle<int> bandMixBounds()
    {
        const auto bounds = timeline().clipBounds (bandMixClipId());
        REQUIRE (! bounds.isEmpty());
        return bounds;
    }

    juce::StringArray audioFilePaths()
    {
        juce::StringArray paths;
        paths.add (fixture->writeSineFile ("Drums.wav", 1.0, 110.0).getFullPathName());
        return paths;
    }

    void mouseDown (juce::Point<int> at) { timeline().mouseDown (mouseEventAt (timeline(), at.toFloat(), leftButton)); }
    void mouseDrag (juce::Point<int> from, juce::Point<int> to) { timeline().mouseDrag (mouseEventAt (timeline(), to.toFloat(), leftButton, from.toFloat())); }
    void mouseUp (juce::Point<int> from, juce::Point<int> to) { timeline().mouseUp (mouseEventAt (timeline(), to.toFloat(), juce::ModifierKeys(), from.toFloat())); }
};

bool pressEscape (MainWindowFixture& f)
{
    return pressKey (*f.window, juce::KeyPress (juce::KeyPress::escapeKey));
}

void pressAndDragGrip (TrackGrip& grip, juce::Point<int> to)
{
    const auto down = grip.getLocalBounds().getCentre().toFloat();
    grip.mouseDown (mouseEventAt (grip, down, leftButton));
    grip.mouseDrag (mouseEventAt (grip, to.toFloat(), leftButton, down));
}

void releaseGrip (TrackGrip& grip, juce::Point<int> at)
{
    grip.mouseUp (mouseEventAt (grip, at.toFloat(), juce::ModifierKeys(), grip.getLocalBounds().getCentre().toFloat()));
}

std::vector<std::string> trackNames (const song::Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}
} // namespace

TEST_CASE ("a Clip drag's preview shows only while it runs; after mouse up the Timeline shows the current Song", "[ui][timeline][gesture]")
{
    GestureFixture f;
    const auto before = f.bandMixBounds();
    std::vector<std::string> editLabels;
    const auto runIntent = f.timeline().onIntent;
    f.timeline().onIntent = [&editLabels, &f, runIntent] (const Intent& reported) {
        const auto* songIntent = std::get_if<SongIntent> (&reported);
        const auto* edit = songIntent != nullptr ? std::get_if<intent::Edit> (songIntent) : nullptr;
        if (edit == nullptr)
            return runIntent (reported);
        editLabels.push_back (editedSong (f.song(), *edit)->label);
    };

    const auto from = before.getCentre();
    const auto to = from.translated (50, 0);
    f.mouseDown (from);
    f.mouseDrag (from, to);
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));

    f.mouseUp (from, to);
    CHECK (editLabels == std::vector<std::string> { "Move Clip" });
    // The edit was reported but not applied: the shown Song is still the current one.
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before);
}

TEST_CASE ("Esc during a Clip drag drops its preview and reports nothing", "[ui][timeline][gesture]")
{
    GestureFixture f;
    const auto songBefore = f.song();
    const auto labelBefore = f.openSong->package().undoLabel();
    const auto before = f.bandMixBounds();
    const auto from = before.getCentre();
    const auto to = from.translated (50, 0);

    core::ScopedLogCapture capture;
    f.mouseDown (from);
    f.mouseDrag (from, to);
    REQUIRE (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));
    CHECK (pressEscape (f));
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before);
    f.mouseDrag (from, to.translated (20, 0));
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before);
    f.mouseUp (from, to);

    CHECK (f.song() == songBefore);
    CHECK (f.openSong->package().undoLabel() == labelBefore);
    CHECK (f.timeline().selectedClip() == f.bandMixClipId());
    const auto cancelled = core::testing::firstEvent (capture, "ui.timeline_gesture_cancelled");
    CHECK (cancelled.at ("gesture") == "clip_drag");
    CHECK (cancelled.at ("reason") == "escape");
}

TEST_CASE ("Esc during a ruler drag stops moving the playhead", "[ui][timeline][gesture]")
{
    GestureFixture f;
    const auto ruler = f.timeline().rulerBounds();
    const auto samplesPerPixel = f.timeline().view().samplesPerPixel;
    const juce::Point<int> from { ruler.getX() + 100, ruler.getCentreY() };
    const juce::Point<int> to { ruler.getX() + 200, ruler.getCentreY() };

    f.mouseDown (from);
    REQUIRE (f.openSong->position() == 100 * samplesPerPixel);
    CHECK (pressEscape (f));
    f.mouseDrag (from, to);
    f.mouseUp (from, to);
    CHECK (f.openSong->position() == 100 * samplesPerPixel);
    CHECK_FALSE (pressEscape (f));
}

TEST_CASE ("Esc during a file drag leaves it hovering, and its drop is still accepted", "[ui][timeline][gesture][drop]")
{
    GestureFixture f;
    const auto paths = f.audioFilePaths();
    std::vector<juce::File> dropped;
    f.timeline().onIntent = [&dropped] (const Intent& reported) {
        for (const auto& path : std::get<intent::ImportFiles> (std::get<SongIntent> (reported)).files)
            dropped.push_back (juce::File (path.value));
    };
    f.timeline().fileDragEnter (paths, 10, 10);
    REQUIRE (f.timeline().isDropHighlighted());
    CHECK_FALSE (f.timeline().cancelGesture ("escape"));
    pressEscape (f);
    CHECK (f.timeline().isDropHighlighted());

    f.timeline().filesDropped (paths, 10, 10);
    CHECK_FALSE (f.timeline().isDropHighlighted());
    CHECK (dropped == std::vector<juce::File> { juce::File (paths[0]) });
}

TEST_CASE ("a mouse down on the Timeline during a grip drag starts nothing, and the grip drag carries on", "[ui][timeline][gesture][track_reorder]")
{
    GestureFixture f;
    REQUIRE (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" });
    const auto position = f.openSong->position();
    auto& grip = f.editHeaderNamed ("Vocals").grip;
    const auto rows = f.timeline().trackRowSpans();
    // Above Band Mix's centre: the first slot.
    const auto dropAt = grip.getLocalPoint (&f.timeline(), juce::Point<int> { 0, rows[0].getStart() + 1 });
    pressAndDragGrip (grip, dropAt);
    REQUIRE (f.timeline().trackDrag().isActive());

    const auto bandMixStart = f.trackNamed ("Band Mix").clips.front().timelineStart;
    const auto before = f.bandMixBounds();
    const auto clip = before.getCentre();
    f.mouseDown (clip);
    f.mouseDrag (clip, clip.translated (50, 0));
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before);
    f.mouseUp (clip, clip.translated (50, 0));
    CHECK_FALSE (f.timeline().selectedClip().has_value());
    const auto ruler = f.timeline().rulerBounds();
    f.mouseDown ({ ruler.getX() + 100, ruler.getCentreY() });
    CHECK (f.openSong->position() == position);
    CHECK (f.timeline().trackDrag().isActive());

    releaseGrip (grip, dropAt);
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Vocals", "Band Mix", "Guitar" });
    CHECK (f.openSong->package().undoLabel() == "Move Track");
    CHECK (f.trackNamed ("Band Mix").clips.front().timelineStart == bandMixStart);
}

TEST_CASE ("a grip drag whose Track is deleted ends, and a mouse down on a Clip selects it again", "[ui][timeline][gesture][track_reorder]")
{
    GestureFixture f;
    auto& grip = f.editHeaderNamed ("Vocals").grip;
    pressAndDragGrip (grip, { 0, 0 });
    REQUIRE (f.timeline().trackDrag().isActive());

    core::ScopedLogCapture capture;
    f.window->songView()->editPage.onDeleteTrack (f.trackNamed ("Vocals").id);
    f.answerDeleteQuestion (true);
    REQUIRE (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar" });
    CHECK_FALSE (f.timeline().trackDrag().isActive());
    const auto cancelled = core::testing::firstEvent (capture, "ui.track_drag_cancelled");
    CHECK (cancelled.at ("page") == "edit");
    CHECK (cancelled.at ("reason") == "track_removed");

    f.mouseDown (f.bandMixBounds().getCentre());
    CHECK (f.timeline().selectedClip() == f.bandMixClipId());
}

TEST_CASE ("a grip press during a Clip drag starts no grip drag, and the Clip drag carries on", "[ui][timeline][gesture][track_reorder]")
{
    GestureFixture f;
    const auto before = f.bandMixBounds();
    const auto from = before.getCentre();
    const auto to = from.translated (50, 0);
    f.mouseDown (from);
    f.mouseDrag (from, to);

    auto& grip = f.editHeaderNamed ("Vocals").grip;
    pressAndDragGrip (grip, { 0, 0 });
    CHECK_FALSE (f.timeline().trackDrag().isActive());
    releaseGrip (grip, { 0, 0 });
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" });

    f.mouseUp (from, to);
    CHECK (f.openSong->package().undoLabel() == "Move Clip");
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));
}

TEST_CASE ("a file drag during a Clip drag is refused, and the Clip drag carries on", "[ui][timeline][gesture][drop]")
{
    GestureFixture f;
    const auto paths = f.audioFilePaths();
    const auto tracksBefore = f.song().tracks.size();
    const auto before = f.bandMixBounds();
    const auto from = before.getCentre();
    const auto to = from.translated (50, 0);
    f.mouseDown (from);
    f.mouseDrag (from, to);

    CHECK_FALSE (f.timeline().isInterestedInFileDrag (paths));
    f.timeline().fileDragEnter (paths, 10, 10);
    CHECK_FALSE (f.timeline().isDropHighlighted());
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));
    f.timeline().fileDragExit (paths);
    f.timeline().filesDropped (paths, 10, 10);
    CHECK (f.song().tracks.size() == tracksBefore);

    f.mouseUp (from, to);
    CHECK (f.openSong->package().undoLabel() == "Move Clip");
    CHECK (f.timeline().isInterestedInFileDrag (paths));
}

TEST_CASE ("a Pass starting during a Clip drag leaves the drag running, and its edit is not applied while the Pass runs", "[ui][timeline][gesture][record]")
{
    GestureFixture f;
    const auto labelBefore = f.openSong->package().undoLabel();
    const auto before = f.bandMixBounds();
    const auto from = before.getCentre();
    const auto to = from.translated (50, 0);
    f.mouseDown (from);
    f.mouseDrag (from, to);
    REQUIRE (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));

    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before.translated (50, 0));
    f.mouseUp (from, to);
    CHECK (f.timeline().clipBounds (f.bandMixClipId()) == before);
    CHECK (f.openSong->package().undoLabel() == labelBefore);
}
