#include "ui/Layout.h"

namespace overdub::ui
{
namespace
{
constexpr int margin = 10;
constexpr int gap = 6;
constexpr int rowGap = 4;
constexpr int modeButtonWidth = 76;
constexpr int transportButtonWidth = 40;
constexpr int clockWidth = 130;
constexpr int undoButtonWidth = 190;
constexpr int importButtonHeight = 30;
} // namespace

MainLayout mainLayout (juce::Rectangle<int> window)
{
    MainLayout layout;
    layout.topBar = window.removeFromTop (topBarHeight);
    layout.page = window;
    layout.noticeLine = window.removeFromBottom (noticeLineHeight);
    return layout;
}

TopBarLayout topBarLayout (juce::Rectangle<int> bar)
{
    TopBarLayout layout;
    auto area = bar.reduced (margin, 8);

    for (auto& button : layout.modeButtons)
    {
        button = area.removeFromLeft (modeButtonWidth);
        area.removeFromLeft (2);
    }

    layout.redo = area.removeFromRight (undoButtonWidth);
    area.removeFromRight (gap);
    layout.undo = area.removeFromRight (undoButtonWidth);

    // The transport group sits in the middle of what is left.
    const auto groupWidth = 3 * transportButtonWidth + 3 * gap + clockWidth;
    auto group = area.withSizeKeepingCentre (groupWidth, area.getHeight());
    layout.rewind = group.removeFromLeft (transportButtonWidth);
    group.removeFromLeft (gap);
    layout.playStop = group.removeFromLeft (transportButtonWidth);
    group.removeFromLeft (gap);
    layout.record = group.removeFromLeft (transportButtonWidth);
    group.removeFromLeft (gap);
    layout.clock = group;
    return layout;
}

RecordPageLayout recordPageLayout (juce::Rectangle<int> page, int numTracks)
{
    RecordPageLayout layout;
    auto area = page.reduced (margin);
    auto column = area.removeFromRight (trackColumnWidth);
    area.removeFromRight (margin);
    layout.stage = area;

    for (int i = 0; i < numTracks; ++i)
    {
        layout.rows.push_back (column.removeFromTop (trackRowHeight));
        column.removeFromTop (rowGap);
    }
    column.removeFromTop (gap);
    layout.importButton = column.removeFromTop (importButtonHeight).removeFromLeft (120);
    return layout;
}

TrackRowLayout trackRowLayout (juce::Rectangle<int> row)
{
    TrackRowLayout layout;
    auto area = row.reduced (6, 6);
    layout.arm = area.removeFromLeft (44);
    area.removeFromLeft (gap);
    layout.monitoring = area.removeFromRight (86);
    area.removeFromRight (gap);
    layout.input = area.removeFromRight (170);
    area.removeFromRight (gap);
    layout.name = area;
    return layout;
}
} // namespace overdub::ui
