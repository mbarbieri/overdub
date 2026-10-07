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
