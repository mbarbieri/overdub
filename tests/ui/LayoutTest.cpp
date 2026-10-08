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

TEST_CASE ("Record mode stacks one row per Track, then the Import button, in the right column", "[ui][layout]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = recordPageLayout (page, 3);

    REQUIRE (layout.rows.size() == 3);
    checkInsideAndApart (page, { layout.stage, layout.rows[0], layout.rows[1], layout.rows[2], layout.importButton });
    CHECK (layout.rows[0].getHeight() == trackRowHeight);
    CHECK (layout.rows[1].getY() > layout.rows[0].getBottom() - 1);
    CHECK (layout.rows[2].getBottom() < layout.importButton.getY());
    CHECK (layout.stage.getRight() < layout.rows[0].getX());
    CHECK (layout.rows[0].getWidth() == trackColumnWidth);

    const auto empty = recordPageLayout (page, 0);
    CHECK (empty.rows.empty());
    CHECK (page.contains (empty.importButton));
}

TEST_CASE ("a Track row puts arm, name, input picker and Monitoring left to right", "[ui][layout]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.input, layout.monitoring });
    CHECK (layout.arm.getX() < layout.name.getX());
    CHECK (layout.name.getX() < layout.input.getX());
    CHECK (layout.input.getX() < layout.monitoring.getX());
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
    const auto layout = timelineLayout (area, 2);

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

TEST_CASE ("a Track header puts the arm button left of the name", "[ui][layout][edit]")
{
    const Rectangle<int> header { 0, 100, laneHeaderWidth, trackLaneHeight };
    const auto layout = trackHeaderLayout (header);
    checkInsideAndApart (header, { layout.arm, layout.name });
    CHECK (layout.arm.getX() < layout.name.getX());
}

TEST_CASE ("the Clip toolbar puts the Clip's details, fade and gain fields, Split and Delete left, zoom and Mixer right", "[ui][layout][edit]")
{
    const Rectangle<int> bar { 0, 52, minimumWindowWidth, clipToolbarHeight };
    const auto layout = clipToolbarLayout (bar);
    checkInsideAndApart (bar, { layout.details, layout.fadeInCaption, layout.fadeInField, layout.fadeOutCaption, layout.fadeOutField, layout.gainCaption, layout.gainField, layout.split, layout.deleteClip, layout.zoomOut, layout.zoomIn, layout.mixerToggle });
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
    CHECK (layout.zoomIn.getRight() <= layout.mixerToggle.getX());
}
