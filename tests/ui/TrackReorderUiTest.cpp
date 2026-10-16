#include "ui/TrackDrag.h"

#include "core/LogEvents.h"
#include "ui/Layout.h"
#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// Reordering Tracks by their grip (issue #138) through the real window: synthesized drags on an
// Edit Track header's grip and a Record row's grip, Esc, a running Pass, unfolded Take Lanes; then
// the Song's Track order, the undo step, and the order the Edit headers, Record rows and Mixer
// strips show.
using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
const juce::ModifierKeys leftButton { juce::ModifierKeys::leftButtonModifier };

std::vector<std::string> trackNames (const song::Song& song)
{
    std::vector<std::string> names;
    for (const auto& track : song.tracks)
        names.push_back (track.name);
    return names;
}

std::vector<song::Id> trackIds (const song::Song& song)
{
    std::vector<song::Id> ids;
    for (const auto& track : song.tracks)
        ids.push_back (track.id);
    return ids;
}

std::vector<song::Id> headerTrackIds (MainWindowFixture& f)
{
    std::vector<song::Id> ids;
    for (const auto& header : f.window->editPage.timeline.headers)
        ids.push_back (header->trackId());
    return ids;
}

std::vector<song::Id> rowTrackIds (MainWindowFixture& f)
{
    std::vector<song::Id> ids;
    for (const auto& row : f.window->recordPage.rows)
        ids.push_back (row->trackId());
    return ids;
}

std::vector<song::Id> stripTrackIds (MainWindowFixture& f)
{
    std::vector<song::Id> ids;
    for (const auto& strip : f.window->editPage.mixer.strips)
        ids.push_back (strip->trackId());
    return ids;
}

// Every view shows the Song's Track order, each page after it is shown again.
void checkEveryViewShowsTheOrder (MainWindowFixture& f)
{
    const auto order = trackIds (f.song());
    REQUIRE (pressKey (*f.window, '2'));
    CHECK (headerTrackIds (f) == order);
    CHECK (stripTrackIds (f) == order);
    REQUIRE (pressKey (*f.window, '1'));
    CHECK (rowTrackIds (f) == order);
}

// A point in the grip's coordinates at the grip's centre x and the page's y.
juce::Point<int> gripPointAt (TrackGrip& grip, juce::Component& page, int pageY)
{
    const auto centre = page.getLocalPoint (&grip, grip.getLocalBounds().getCentre());
    return grip.getLocalPoint (&page, juce::Point<int> { centre.x, pageY });
}

// Presses the grip at its centre and drags it to the page's y, without releasing.
void pressAndDragGrip (TrackGrip& grip, juce::Component& page, int pageY)
{
    const auto down = grip.getLocalBounds().getCentre().toFloat();
    grip.mouseDown (mouseEventAt (grip, down, leftButton));
    grip.mouseDrag (mouseEventAt (grip, gripPointAt (grip, page, pageY).toFloat(), leftButton, down));
}

void releaseGrip (TrackGrip& grip, juce::Component& page, int pageY)
{
    const auto down = grip.getLocalBounds().getCentre().toFloat();
    grip.mouseUp (mouseEventAt (grip, gripPointAt (grip, page, pageY).toFloat(), juce::ModifierKeys(), down));
}

// A whole grip drag released at the page's y.
void dragGripTo (TrackGrip& grip, juce::Component& page, int pageY)
{
    pressAndDragGrip (grip, page, pageY);
    releaseGrip (grip, page, pageY);
}

std::size_t countEvents (const core::ScopedLogCapture& capture, const std::string& name)
{
    return (std::size_t) std::ranges::count_if (capture.events(), [&name] (const auto& event) { return event.at ("event") == name; });
}

int centreOf (juce::Range<int> span)
{
    return (span.getStart() + span.getEnd()) / 2;
}
} // namespace

TEST_CASE ("dragging an Edit header's grip between two other Tracks moves it as one Move Track undo step", "[ui][track_reorder]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    REQUIRE (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" });
    const auto songBefore = f.song();
    const auto vocalsId = f.trackNamed ("Vocals").id;
    const auto rows = timeline.trackRowSpans();
    REQUIRE (rows.size() == 3);

    core::ScopedLogCapture capture;
    // Just above Guitar's centre: between Band Mix and Guitar.
    dragGripTo (f.editHeaderNamed ("Vocals").grip, timeline, centreOf (rows[1]) - 1);

    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Vocals", "Guitar" });
    CHECK (f.openSong->package().undoLabel() == "Move Track");
    const auto moved = core::testing::firstEvent (capture, "ui.track_moved");
    CHECK (moved.at ("track_id") == vocalsId);
    CHECK (moved.at ("to") == 1);
    checkEveryViewShowsTheOrder (f);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song() == songBefore);
    checkEveryViewShowsTheOrder (f);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Vocals", "Guitar" });
    checkEveryViewShowsTheOrder (f);
}

TEST_CASE ("dragging a Record row's grip between two other Tracks moves it as one Move Track undo step", "[ui][track_reorder]")
{
    MainWindowFixture f;
    auto& page = f.window->recordPage;
    REQUIRE (isShown (page));
    const auto songBefore = f.song();
    const auto rows = page.trackRowSpans();
    REQUIRE (rows.size() == 3);

    // Just below Guitar's centre: between Guitar and Vocals.
    dragGripTo (f.rowNamed ("Band Mix").grip, page, centreOf (rows[1]) + 1);

    CHECK (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Band Mix", "Vocals" });
    CHECK (f.openSong->package().undoLabel() == "Move Track");
    checkEveryViewShowsTheOrder (f);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    CHECK (f.song() == songBefore);
    checkEveryViewShowsTheOrder (f);

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier));
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Band Mix", "Vocals" });
    checkEveryViewShowsTheOrder (f);
}

TEST_CASE ("a grip dragged to the top or the bottom moves the Track first or last", "[ui][track_reorder]")
{
    MainWindowFixture f;
    auto& page = f.window->recordPage;
    dragGripTo (f.rowNamed ("Vocals").grip, page, 0);
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Vocals", "Band Mix", "Guitar" });

    dragGripTo (f.rowNamed ("Vocals").grip, page, page.getHeight());
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" });

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    // Above the Camera lane, the Track still lands first: drops only land between Tracks.
    dragGripTo (f.editHeaderNamed ("Guitar").grip, timeline, timeline.cameraLaneBounds().getY());
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Band Mix", "Vocals" });
}

TEST_CASE ("a grip dropped back in its own slot changes nothing and adds no undo step", "[ui][track_reorder]")
{
    MainWindowFixture f;
    const auto songBefore = f.song();
    const auto labelBefore = f.openSong->package().undoLabel();
    auto& page = f.window->recordPage;
    const auto recordRows = page.trackRowSpans();

    core::ScopedLogCapture capture;
    // Guitar between Band Mix's and Vocals' centres stays second.
    dragGripTo (f.rowNamed ("Guitar").grip, page, centreOf (recordRows[0]) + 1);
    dragGripTo (f.rowNamed ("Guitar").grip, page, centreOf (recordRows[2]));

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    dragGripTo (f.editHeaderNamed ("Band Mix").grip, timeline, centreOf (timeline.trackRowSpans()[1]));

    CHECK (f.song() == songBefore);
    CHECK (f.openSong->package().undoLabel() == labelBefore);
    CHECK (countEvents (capture, "ui.track_dropped") == 3);
    CHECK (countEvents (capture, "ui.track_moved") == 0);
}

TEST_CASE ("while a grip drags, the row follows the pointer and an insertion line marks the drop slot", "[ui][track_reorder]")
{
    MainWindowFixture f;
    auto& page = f.window->recordPage;
    auto& grip = f.rowNamed ("Band Mix").grip;
    const auto rows = page.trackRowSpans();
    const auto bandMixRow = f.rowNamed ("Band Mix").getBounds();
    const auto grabY = page.getLocalPoint (&grip, grip.getLocalBounds().getCentre()).y;

    const auto pointerY = centreOf (rows[2]) + 1;
    pressAndDragGrip (grip, page, pointerY);

    REQUIRE (page.trackDrag().isActive());
    CHECK (page.trackDrag().ghostBounds() == bandMixRow.translated (0, pointerY - grabY));
    CHECK (page.trackDrag().dropIndex() == std::optional<std::size_t> { 2 });
    CHECK (page.trackDrag().dropLineY() == std::optional<int> { rows[2].getEnd() });
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" }); // nothing moves before the drop

    releaseGrip (grip, page, pointerY);
    CHECK_FALSE (page.trackDrag().isActive());
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Vocals", "Band Mix" });
}

TEST_CASE ("Esc during a grip drag cancels it and leaves the order unchanged", "[ui][track_reorder]")
{
    MainWindowFixture f;
    const auto songBefore = f.song();
    const auto labelBefore = f.openSong->package().undoLabel();

    auto& page = f.window->recordPage;
    auto& rowGrip = f.rowNamed ("Vocals").grip;
    pressAndDragGrip (rowGrip, page, 0);
    REQUIRE (page.trackDrag().isActive());
    CHECK (pressKey (*f.window, juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK_FALSE (page.trackDrag().isActive());
    releaseGrip (rowGrip, page, 0);

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    auto& headerGrip = f.editHeaderNamed ("Band Mix").grip;
    pressAndDragGrip (headerGrip, timeline, timeline.getHeight());
    REQUIRE (timeline.trackDrag().isActive());
    core::ScopedLogCapture capture;
    CHECK (pressKey (*f.window, juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK_FALSE (timeline.trackDrag().isActive());
    const auto cancelled = core::testing::firstEvent (capture, "ui.track_drag_cancelled");
    CHECK (cancelled.at ("page") == "edit");
    CHECK (cancelled.at ("reason") == "escape");
    releaseGrip (headerGrip, timeline, timeline.getHeight());

    CHECK (f.song() == songBefore);
    CHECK (f.openSong->package().undoLabel() == labelBefore);
}

TEST_CASE ("while a Pass runs, the grips are disabled and a grip drag does nothing on either page", "[ui][track_reorder]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    const auto namesBefore = trackNames (f.song());

    auto& page = f.window->recordPage;
    auto& rowGrip = f.rowNamed ("Vocals").grip;
    CHECK_FALSE (rowGrip.isEnabled());
    pressAndDragGrip (rowGrip, page, 0);
    CHECK_FALSE (page.trackDrag().isActive());
    releaseGrip (rowGrip, page, 0);
    CHECK (trackNames (f.song()) == namesBefore);

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    auto& headerGrip = f.editHeaderNamed ("Vocals").grip;
    CHECK_FALSE (headerGrip.isEnabled());
    pressAndDragGrip (headerGrip, timeline, 0);
    CHECK_FALSE (timeline.trackDrag().isActive());
    releaseGrip (headerGrip, timeline, 0);
    CHECK (trackNames (f.song()) == namesBefore);
}

TEST_CASE ("a Pass starting during a grip drag cancels the drag", "[ui][track_reorder]")
{
    MainWindowFixture f;
    auto& page = f.window->recordPage;
    auto& grip = f.rowNamed ("Vocals").grip;
    pressAndDragGrip (grip, page, 0);
    REQUIRE (page.trackDrag().isActive());

    REQUIRE (pressKey (*f.window, 'r'));
    REQUIRE (f.openSong->isRecording());
    CHECK_FALSE (page.trackDrag().isActive());
    releaseGrip (grip, page, 0);
    CHECK (trackNames (f.song()) == std::vector<std::string> { "Band Mix", "Guitar", "Vocals" });
}

TEST_CASE ("unfolded Take Lanes stay under their Track after it moves", "[ui][track_reorder][take_lanes]")
{
    MainWindowFixture f;
    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (300);
    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE (f.song().passes.size() == 1);

    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    const auto guitarId = f.trackNamed ("Guitar").id;
    timeline.setTakeLanesUnfolded (guitarId, true);
    const auto lanes = timeline.takeLanesShown (guitarId);
    REQUIRE (lanes.size() == 1);

    // Guitar (with its Take Lane) above Band Mix's centre: it goes first.
    dragGripTo (f.editHeaderNamed ("Guitar").grip, timeline, centreOf (timeline.trackRowSpans()[0]) - 1);

    REQUIRE (trackNames (f.song()) == std::vector<std::string> { "Guitar", "Band Mix", "Vocals" });
    CHECK (timeline.areTakeLanesUnfolded (guitarId));
    const auto rows = timeline.trackRowSpans();
    CHECK (rows[0].getLength() == trackLaneHeight + takeLaneHeight);
    CHECK (rows[1].getLength() == trackLaneHeight);
    const auto lane = timeline.takeLaneBounds (lanes[0].takeId);
    CHECK (lane.getY() == rows[0].getStart() + trackLaneHeight);
    CHECK (lane.getBottom() == rows[1].getStart());
    // Guitar's header is first, over its own lane, and Band Mix's comes after the Take Lane.
    CHECK (headerTrackIds (f) == trackIds (f.song()));
    CHECK (f.window->editPage.timeline.getLocalArea (&f.editHeaderNamed ("Band Mix"), f.editHeaderNamed ("Band Mix").getLocalBounds()).getY() == rows[1].getStart());
}
