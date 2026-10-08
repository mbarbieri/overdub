#include "ui/Layout.h"

#include <algorithm>

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

int clampMixerHeight (int requested, int pageHeight)
{
    const auto largest = pageHeight - clipToolbarHeight - mixerDividerHeight - minimumTimelineHeight;
    return std::max (minimumMixerHeight, std::min (requested, largest));
}

EditPageLayout editPageLayout (juce::Rectangle<int> page, bool mixerVisible, int mixerHeight)
{
    EditPageLayout layout;
    auto area = page;
    layout.clipToolbar = area.removeFromTop (clipToolbarHeight);
    if (mixerVisible)
    {
        layout.mixer = area.removeFromBottom (clampMixerHeight (mixerHeight, page.getHeight()));
        layout.mixerDivider = area.removeFromBottom (mixerDividerHeight);
    }
    layout.timeline = area;
    return layout;
}

ClipToolbarLayout clipToolbarLayout (juce::Rectangle<int> bar)
{
    ClipToolbarLayout layout;
    auto area = bar.reduced (margin, 5);
    layout.mixerToggle = area.removeFromRight (76);
    area.removeFromRight (gap);
    layout.zoomIn = area.removeFromRight (32);
    area.removeFromRight (2);
    layout.zoomOut = area.removeFromRight (32);
    area.removeFromRight (gap * 2);
    constexpr int buttonWidth = 80;
    constexpr int fieldWidth = 64;
    constexpr int captionGap = 2;
    constexpr int fieldsWidth = 48 + 56 + 32 + 3 * (captionGap + fieldWidth) + 2 * gap;
    layout.details = area.removeFromLeft (std::max (0, area.getWidth() - fieldsWidth - 2 * buttonWidth - 3 * gap));
    area.removeFromLeft (gap);
    layout.fadeInCaption = area.removeFromLeft (48);
    area.removeFromLeft (captionGap);
    layout.fadeInField = area.removeFromLeft (fieldWidth);
    area.removeFromLeft (gap);
    layout.fadeOutCaption = area.removeFromLeft (56);
    area.removeFromLeft (captionGap);
    layout.fadeOutField = area.removeFromLeft (fieldWidth);
    area.removeFromLeft (gap);
    layout.gainCaption = area.removeFromLeft (32);
    area.removeFromLeft (captionGap);
    layout.gainField = area.removeFromLeft (fieldWidth);
    area.removeFromLeft (gap);
    layout.split = area.removeFromLeft (buttonWidth);
    area.removeFromLeft (gap);
    layout.deleteClip = area.removeFromLeft (buttonWidth);
    return layout;
}

TimelineLayout timelineLayout (juce::Rectangle<int> area, int numTracks)
{
    TimelineLayout layout;
    auto rest = area;
    auto scrollRow = rest.removeFromBottom (timelineScrollBarHeight);
    scrollRow.removeFromLeft (laneHeaderWidth);
    layout.scrollBar = scrollRow;

    auto rulerRow = rest.removeFromTop (rulerHeight);
    layout.rulerCorner = rulerRow.removeFromLeft (laneHeaderWidth);
    layout.ruler = rulerRow;

    auto cameraRow = rest.removeFromTop (cameraLaneHeight);
    layout.cameraHeader = cameraRow.removeFromLeft (laneHeaderWidth);
    layout.cameraLane = cameraRow;

    for (int i = 0; i < numTracks; ++i)
    {
        auto row = juce::Rectangle<int> (rest.getX(), rest.getY() + i * trackLaneHeight, rest.getWidth(), trackLaneHeight);
        layout.trackHeaders.push_back (row.removeFromLeft (laneHeaderWidth));
        layout.trackLanes.push_back (row);
    }
    return layout;
}

TrackHeaderLayout trackHeaderLayout (juce::Rectangle<int> header)
{
    TrackHeaderLayout layout;
    auto area = header.reduced (6, 6);
    layout.arm = area.removeFromLeft (40).withSizeKeepingCentre (40, 24);
    area.removeFromLeft (gap);
    layout.name = area;
    return layout;
}
} // namespace overdub::ui
