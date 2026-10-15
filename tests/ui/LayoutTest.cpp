#include "ui/Layout.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using juce::Rectangle;
using namespace overdub::ui;

namespace
{
void checkInsideAndApart (const Rectangle<int>& area, const std::vector<Rectangle<int>>& parts)
{
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        INFO ("part " << i << " " << parts[i].toString());
        CHECK (! parts[i].isEmpty());
        CHECK (area.contains (parts[i]));
        for (std::size_t j = i + 1; j < parts.size(); ++j)
            CHECK (! parts[i].intersects (parts[j]));
    }
}
} // namespace

TEST_CASE ("the window is a top bar over the page, with the notice line along the page's bottom", "[ui][layout]")
{
    const Rectangle<int> window { 0, 0, minimumWindowWidth, minimumWindowHeight };
    const auto layout = mainLayout (window);

    CHECK (layout.topBar == Rectangle<int> { 0, 0, minimumWindowWidth, topBarHeight });
    CHECK (layout.page == Rectangle<int> { 0, topBarHeight, minimumWindowWidth, minimumWindowHeight - topBarHeight });
    CHECK (layout.noticeLine.getBottom() == window.getBottom());
    CHECK (layout.page.contains (layout.noticeLine));
}

TEST_CASE ("the top bar's controls fit side by side at the smallest window width", "[ui][layout]")
{
    const Rectangle<int> bar { 0, 0, minimumWindowWidth, topBarHeight };
    const auto layout = topBarLayout (bar);

    checkInsideAndApart (bar, { layout.modeButtons[0], layout.modeButtons[1], layout.modeButtons[2], layout.rewind, layout.playStop, layout.record, layout.clock, layout.undo, layout.redo });
    CHECK (layout.modeButtons[0].getX() < layout.modeButtons[1].getX());
    CHECK (layout.modeButtons[2].getRight() < layout.rewind.getX());
    CHECK (layout.clock.getRight() < layout.undo.getX());
    CHECK (layout.redo.getRight() <= bar.getRight());
}

TEST_CASE ("Record mode stacks one row per Track, the Import button and the Passes list in the right column", "[ui][layout]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = recordPageLayout (page, 3);

    REQUIRE (layout.rows.size() == 3);
    checkInsideAndApart (page, { layout.preRollCaption, layout.preRollPicker, layout.cameraArm, layout.cameraPicker, layout.stage, layout.rows[0], layout.rows[1], layout.rows[2], layout.addTrackButton, layout.importButton, layout.passes, layout.lookAhead, layout.overview });
    CHECK (layout.rows[0].getHeight() == trackRowHeight);
    CHECK (layout.rows[1].getY() > layout.rows[0].getBottom() - 1);
    CHECK (layout.rows[2].getBottom() < layout.importButton.getY());
    CHECK (layout.addTrackButton.getY() == layout.importButton.getY());
    CHECK (layout.addTrackButton.getRight() < layout.importButton.getX());
    CHECK (layout.importButton.getBottom() < layout.passes.getY());
    CHECK (layout.passes.getWidth() == trackColumnWidth);
    CHECK (layout.stage.getRight() < layout.rows[0].getX());
    CHECK (layout.rows[0].getWidth() == trackColumnWidth);

    // The pre-roll picker sits above the stage; the strip and the overview span the page's width
    // under everything else.
    CHECK (layout.preRollCaption.getRight() < layout.preRollPicker.getX());
    CHECK (layout.preRollPicker.getBottom() < layout.stage.getY());
    // The camera arm and picker sit above the stage at its right end (issue #35).
    CHECK (layout.preRollPicker.getRight() < layout.cameraArm.getX());
    CHECK (layout.cameraArm.getRight() < layout.cameraPicker.getX());
    CHECK (layout.cameraPicker.getRight() == layout.stage.getRight());
    CHECK (layout.cameraArm.getBottom() < layout.stage.getY());
    CHECK (layout.cameraPicker.getWidth() >= 160);
    CHECK (layout.lookAhead.getHeight() == lookAheadStripHeight);
    CHECK (layout.overview.getHeight() == songOverviewHeight);
    CHECK (layout.lookAhead.getX() == layout.stage.getX());
    CHECK (layout.lookAhead.getRight() == layout.rows[0].getRight());
    CHECK (layout.stage.getBottom() < layout.lookAhead.getY());
    CHECK (layout.passes.getBottom() < layout.lookAhead.getY());
    CHECK (layout.lookAhead.getBottom() < layout.overview.getY());

    const auto empty = recordPageLayout (page, 0);
    CHECK (empty.rows.empty());
    CHECK (page.contains (empty.addTrackButton));
    CHECK (page.contains (empty.importButton));
}

TEST_CASE ("a Track row puts arm, name, M, S, input picker and Monitoring left to right", "[ui][layout]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.mute, layout.solo, layout.input, layout.monitoring });
    CHECK (layout.arm.getX() < layout.name.getX());
    CHECK (layout.name.getX() < layout.mute.getX());
    CHECK (layout.mute.getX() < layout.solo.getX());
    CHECK (layout.solo.getX() < layout.input.getX());
    CHECK (layout.name.getWidth() >= 80);
    CHECK (layout.input.getX() < layout.monitoring.getX());
}

TEST_CASE ("a Track row puts its Insert Chain under the name", "[ui][layout][inserts]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.mute, layout.solo, layout.input, layout.monitoring, layout.insertChain });
    CHECK (layout.insertChain.getY() >= layout.name.getBottom());
    CHECK (layout.insertChain.getX() == layout.name.getX());
    CHECK (layout.insertChain.getHeight() == insertChipHeight);
    // "+ Insert" and three chips fit.
    CHECK (layout.insertChain.getWidth() >= insertAddButtonWidth + 3 * insertChipWidth);
    CHECK (layout.monitoring.getWidth() >= 110);
}

TEST_CASE ("a horizontal Insert Chain puts + Insert first, then the chips left to right, cutting off what does not fit", "[ui][layout][inserts]")
{
    const Rectangle<int> area { 50, 40, 400, insertChipHeight };
    const auto layout = insertChainLayout (area, 5, false);

    REQUIRE (layout.chips.size() == 5);
    CHECK (layout.addButton.getX() == area.getX());
    CHECK (layout.addButton.getWidth() == insertAddButtonWidth);
    CHECK (layout.chips[0].getX() > layout.addButton.getRight());
    CHECK (layout.chips[1].getX() > layout.chips[0].getRight());
    CHECK (layout.chips[0].getWidth() == insertChipWidth);
    checkInsideAndApart (area, { layout.addButton, layout.chips[0], layout.chips[1] });
    CHECK (layout.chips[3].isEmpty());
    CHECK (layout.chips[4].isEmpty());
}

TEST_CASE ("a vertical Insert Chain stacks the chips from the top and keeps + Insert at the bottom", "[ui][layout][inserts]")
{
    const Rectangle<int> area { 10, 100, 98, mixerInsertsHeight };
    const auto layout = insertChainLayout (area, 4, true);

    REQUIRE (layout.chips.size() == 4);
    CHECK (layout.chips[0].getY() == area.getY());
    CHECK (layout.chips[0].getWidth() == area.getWidth());
    CHECK (layout.chips[0].getHeight() == insertChipHeight);
    CHECK (layout.chips[1].getY() > layout.chips[0].getBottom());
    CHECK (layout.addButton.getBottom() == area.getBottom());
    checkInsideAndApart (area, { layout.chips[0], layout.chips[1], layout.chips[2], layout.addButton });
    CHECK (layout.chips[3].isEmpty()); // three show above + Insert
}

TEST_CASE ("an Insert chip is its name, then bypass and remove at its right end", "[ui][layout][inserts]")
{
    const Rectangle<int> chip { 0, 0, insertChipWidth, insertChipHeight };
    const auto layout = insertChipLayout (chip);
    checkInsideAndApart (chip, { layout.name, layout.bypass, layout.remove });
    CHECK (layout.name.getRight() <= layout.bypass.getX());
    CHECK (layout.bypass.getRight() <= layout.remove.getX());
    CHECK (layout.remove.getRight() == chip.getRight());
}

TEST_CASE ("a dragged Insert chip lands before the first other chip whose centre is past the drop point", "[ui][layout][inserts]")
{
    const auto vertical = insertChainLayout ({ 0, 0, 98, 200 }, 3, true).chips;
    // Dragging the first chip below the second's centre puts it second; below the third's, last.
    CHECK (insertDropIndex (vertical, 0, vertical[1].getCentre().translated (0, 1), true) == 1);
    CHECK (insertDropIndex (vertical, 0, vertical[2].getCentre().translated (0, 1), true) == 2);
    CHECK (insertDropIndex (vertical, 0, { 5, 1000 }, true) == 2);
    // The last chip dragged above the first's centre goes first.
    CHECK (insertDropIndex (vertical, 2, vertical[0].getCentre().translated (0, -1), true) == 0);
    CHECK (insertDropIndex (vertical, 2, { 5, -50 }, true) == 0);
    // Dropped where it was: the same place.
    CHECK (insertDropIndex (vertical, 1, vertical[1].getCentre(), true) == 1);

    const auto horizontal = insertChainLayout ({ 0, 0, 600, insertChipHeight }, 3, false).chips;
    CHECK (insertDropIndex (horizontal, 0, horizontal[2].getCentre().translated (1, 0), false) == 2);
    CHECK (insertDropIndex (horizontal, 2, horizontal[0].getTopLeft(), false) == 0);
}

TEST_CASE ("Edit mode stacks the Clip toolbar, the timeline, the divider and the Mixer region", "[ui][layout][edit]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = editPageLayout (page, true, 220);

    checkInsideAndApart (page, { layout.clipToolbar, layout.timeline, layout.mixerDivider, layout.mixer });
    CHECK (layout.clipToolbar.getHeight() == clipToolbarHeight);
    CHECK (layout.timeline.getY() == layout.clipToolbar.getBottom());
    CHECK (layout.mixerDivider.getY() == layout.timeline.getBottom());
    CHECK (layout.mixer.getY() == layout.mixerDivider.getBottom());
    CHECK (layout.mixer.getHeight() == 220);
    CHECK (layout.mixer.getBottom() == page.getBottom());
    CHECK (layout.mixer.getWidth() == page.getWidth());
}

TEST_CASE ("a hidden Mixer region gives the whole page below the toolbar to the timeline", "[ui][layout][edit]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = editPageLayout (page, false, 220);

    CHECK (layout.mixer.isEmpty());
    CHECK (layout.mixerDivider.isEmpty());
    CHECK (layout.timeline.getBottom() == page.getBottom());
}

TEST_CASE ("the Mixer region's height stays between its minimum and what leaves the timeline room", "[ui][layout][edit]")
{
    const int pageHeight = 700;
    CHECK (clampMixerHeight (10, pageHeight) == minimumMixerHeight);
    CHECK (clampMixerHeight (300, pageHeight) == 300);
    CHECK (clampMixerHeight (5000, pageHeight) == pageHeight - clipToolbarHeight - mixerDividerHeight - minimumTimelineHeight);

    const Rectangle<int> page { 0, 0, 1200, pageHeight };
    CHECK (editPageLayout (page, true, 5000).timeline.getHeight() == minimumTimelineHeight);
}

TEST_CASE ("the timeline has a ruler, the Camera lane, then one lane per Track, each with its header", "[ui][layout][edit]")
{
    const Rectangle<int> area { 0, 88, 1200, 500 };
    const auto layout = timelineLayout (area, { 0, 0 });

    REQUIRE (layout.trackHeaders.size() == 2);
    REQUIRE (layout.trackLanes.size() == 2);
    checkInsideAndApart (area, { layout.rulerCorner, layout.ruler, layout.cameraHeader, layout.cameraLane, layout.trackHeaders[0], layout.trackLanes[0], layout.trackHeaders[1], layout.trackLanes[1], layout.scrollBar });
    CHECK (layout.ruler.getX() == area.getX() + laneHeaderWidth);
    CHECK (layout.ruler.getHeight() == rulerHeight);
    CHECK (layout.cameraLane.getY() == layout.ruler.getBottom());
    CHECK (layout.cameraLane.getHeight() == cameraLaneHeight);
    CHECK (layout.trackLanes[0].getY() == layout.cameraLane.getBottom());
    CHECK (layout.trackLanes[0].getHeight() == trackLaneHeight);
    CHECK (layout.trackLanes[1].getY() == layout.trackLanes[0].getBottom());
    CHECK (layout.trackHeaders[1].getY() == layout.trackLanes[1].getY());
    CHECK (layout.trackHeaders[0].getWidth() == laneHeaderWidth);
    CHECK (layout.trackLanes[0].getX() == layout.ruler.getX());
    CHECK (layout.trackLanes[0].getRight() == area.getRight());
    CHECK (layout.scrollBar.getBottom() == area.getBottom());
    CHECK (layout.scrollBar.getX() == layout.ruler.getX());
}

TEST_CASE ("a Track header puts the arm button left of the name, and M, S and × right of the Takes toggle", "[ui][layout][edit]")
{
    const Rectangle<int> header { 0, 100, laneHeaderWidth, trackLaneHeight };
    const auto layout = trackHeaderLayout (header);
    checkInsideAndApart (header, { layout.arm, layout.name, layout.takesToggle, layout.mute, layout.solo, layout.deleteButton });
    CHECK (layout.arm.getX() < layout.name.getX());
    CHECK (layout.takesToggle.getY() >= layout.arm.getBottom());
    CHECK (layout.mute.getY() == layout.takesToggle.getY());
    CHECK (layout.takesToggle.getRight() < layout.mute.getX());
    CHECK (layout.mute.getRight() < layout.solo.getX());
    CHECK (layout.deleteButton.getY() == layout.solo.getY());
    CHECK (layout.solo.getRight() < layout.deleteButton.getX());
}

TEST_CASE ("the Mixer puts one strip per Track from the left and the Master strip at the right edge", "[ui][layout][mixer]")
{
    const Rectangle<int> mixer { 0, 500, 1200, defaultMixerHeight };
    const auto layout = mixerLayout (mixer, 4);
    REQUIRE (layout.strips.size() == 4);
    auto parts = layout.strips;
    parts.push_back (layout.master);
    checkInsideAndApart (mixer, parts);
    for (std::size_t i = 1; i < layout.strips.size(); ++i)
    {
        CHECK (layout.strips[i].getX() > layout.strips[i - 1].getRight());
        CHECK (layout.strips[i].getWidth() == mixerStripWidth);
    }
    CHECK (layout.master.getX() > layout.strips.back().getRight());
    CHECK (mixer.getRight() - layout.master.getRight() < 12);

    // Strips that do not fit before the Master are cut off: none overlaps it.
    const auto crowded = mixerLayout (mixer, 30);
    for (const auto& strip : crowded.strips)
        CHECK ((strip.isEmpty() || strip.getRight() < crowded.master.getX()));
    CHECK (! crowded.strips.front().isEmpty());
    CHECK (crowded.strips.back().isEmpty());
}

TEST_CASE ("a Mixer strip stacks name, pan, the Insert Chain, fader beside its meter, the dB value, then M and S", "[ui][layout][mixer]")
{
    const Rectangle<int> strip { 10, 500, mixerStripWidth, defaultMixerHeight - 12 };
    const auto layout = mixerStripLayout (strip);
    checkInsideAndApart (strip, { layout.name, layout.pan, layout.inserts, layout.fader, layout.meter, layout.value, layout.mute, layout.solo });
    CHECK (layout.name.getBottom() <= layout.pan.getY());
    CHECK (layout.pan.getBottom() <= layout.inserts.getY());
    CHECK (layout.inserts.getHeight() == mixerInsertsHeight);
    CHECK (layout.inserts.getBottom() <= layout.fader.getY());
    CHECK (layout.fader.getRight() <= layout.meter.getX());
    CHECK (layout.meter.getY() == layout.fader.getY());
    CHECK (layout.meter.getHeight() == layout.fader.getHeight());
    CHECK (layout.fader.getBottom() <= layout.value.getY());
    CHECK (layout.value.getBottom() <= layout.mute.getY());
    CHECK (layout.mute.getRight() < layout.solo.getX());
    CHECK (layout.fader.getHeight() >= 60);
    // An Insert chip's name has room for about ten characters.
    CHECK (insertChipLayout (layout.inserts.withHeight (insertChipHeight)).name.getWidth() >= 60);
}

TEST_CASE ("a Track's unfolded Take Lanes sit under its lane, before the next Track", "[ui][layout][take_lanes]")
{
    const Rectangle<int> area { 0, 88, 1200, 600 };
    const auto layout = timelineLayout (area, { 3, 0 });

    REQUIRE (layout.takeLanes.size() == 2);
    REQUIRE (layout.takeLanes[0].size() == 3);
    CHECK (layout.takeLanes[1].empty());
    CHECK (layout.takeLanes[0][0].getY() == layout.trackLanes[0].getBottom());
    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK (layout.takeLanes[0][i].getHeight() == takeLaneHeight);
        CHECK (layout.takeLanes[0][i].getX() == layout.trackLanes[0].getX());
        CHECK (layout.takeLaneHeaders[0][i].getY() == layout.takeLanes[0][i].getY());
        CHECK (layout.takeLaneHeaders[0][i].getWidth() == laneHeaderWidth);
    }
    CHECK (layout.takeLanes[0][1].getY() == layout.takeLanes[0][0].getBottom());
    CHECK (layout.trackLanes[1].getY() == layout.takeLanes[0][2].getBottom());
}

TEST_CASE ("the Passes button sits in the timeline's ruler corner and its panel opens under it", "[ui][layout][take_lanes]")
{
    const Rectangle<int> page { 0, 52, 1200, 700 };
    const auto edit = editPageLayout (page, true, defaultMixerHeight);
    CHECK (edit.timeline.contains (edit.passesButton));
    CHECK (edit.passesButton.getBottom() <= edit.timeline.getY() + rulerHeight);
    CHECK (edit.passesButton.getRight() <= edit.timeline.getX() + laneHeaderWidth);

    const auto bounds = passesPanelBounds (page, edit.passesButton, 3);
    CHECK (page.contains (bounds));
    CHECK (bounds.getY() > edit.passesButton.getBottom());
    const auto panel = passesPanelLayout (bounds.withZeroOrigin(), 3);
    REQUIRE (panel.rows.size() == 3);
    checkInsideAndApart (bounds.withZeroOrigin(), { panel.heading, panel.rows[0], panel.rows[1], panel.rows[2] });

    const auto row = passRowLayout (panel.rows[0]);
    checkInsideAndApart (panel.rows[0], { row.title, row.status, row.bringBack });
    CHECK (row.title.getRight() <= row.status.getX());
    CHECK (row.status.getRight() <= row.bringBack.getX());
}

TEST_CASE ("the Clip toolbar puts the Clip's details, fade and gain fields, Split and Delete left, zoom and Mixer right", "[ui][layout][edit]")
{
    const Rectangle<int> bar { 0, 52, minimumWindowWidth, clipToolbarHeight };
    const auto layout = clipToolbarLayout (bar);
    checkInsideAndApart (bar, { layout.details, layout.fadeInCaption, layout.fadeInField, layout.fadeOutCaption, layout.fadeOutField, layout.gainCaption, layout.gainField, layout.split, layout.deleteClip, layout.zoomOut, layout.zoomIn, layout.videoToggle, layout.mixerToggle });
    CHECK (layout.details.getWidth() >= 200);
    CHECK (layout.details.getRight() <= layout.fadeInCaption.getX());
    CHECK (layout.fadeInCaption.getRight() <= layout.fadeInField.getX());
    CHECK (layout.fadeInField.getRight() <= layout.fadeOutCaption.getX());
    CHECK (layout.fadeOutCaption.getRight() <= layout.fadeOutField.getX());
    CHECK (layout.fadeOutField.getRight() <= layout.gainCaption.getX());
    CHECK (layout.gainCaption.getRight() <= layout.gainField.getX());
    CHECK (layout.gainField.getRight() <= layout.split.getX());
    CHECK (layout.split.getRight() <= layout.deleteClip.getX());
    CHECK (layout.deleteClip.getRight() <= layout.zoomOut.getX());
    CHECK (layout.zoomOut.getRight() <= layout.zoomIn.getX());
    CHECK (layout.zoomIn.getRight() <= layout.videoToggle.getX());
    CHECK (layout.videoToggle.getRight() <= layout.mixerToggle.getX());
}
