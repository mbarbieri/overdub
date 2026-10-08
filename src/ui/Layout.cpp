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
    layout.overview = area.removeFromBottom (songOverviewHeight);
    area.removeFromBottom (gap);
    layout.lookAhead = area.removeFromBottom (lookAheadStripHeight);
    area.removeFromBottom (margin);

    auto column = area.removeFromRight (trackColumnWidth);
    area.removeFromRight (margin);
    auto stageBar = area.removeFromTop (stageBarHeight);
    layout.preRollCaption = stageBar.removeFromLeft (60);
    stageBar.removeFromLeft (gap);
    layout.preRollPicker = stageBar.removeFromLeft (90);
    area.removeFromTop (gap);
    layout.stage = area;

    for (int i = 0; i < numTracks; ++i)
    {
        layout.rows.push_back (column.removeFromTop (trackRowHeight));
        column.removeFromTop (rowGap);
    }
    column.removeFromTop (gap);
    layout.importButton = column.removeFromTop (importButtonHeight).removeFromLeft (120);
    column.removeFromTop (margin);
    layout.passes = column;
    return layout;
}

TrackRowLayout trackRowLayout (juce::Rectangle<int> row)
{
    TrackRowLayout layout;
    auto area = row.reduced (6, 6);
    const auto armColumn = area.removeFromLeft (44);
    layout.arm = armColumn.withHeight (28);
    area.removeFromLeft (gap);
    auto bottom = area.removeFromBottom (22);
    area.removeFromBottom (4);
    layout.insertButton = bottom.removeFromLeft (72);
    bottom.removeFromLeft (gap);
    layout.inserts = bottom;
    layout.monitoring = area.removeFromRight (86);
    area.removeFromRight (gap);
    layout.input = area.removeFromRight (150);
    area.removeFromRight (gap);
    layout.solo = area.removeFromRight (26);
    area.removeFromRight (2);
    layout.mute = area.removeFromRight (26);
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
    layout.passesButton = juce::Rectangle<int> (area.getX(), area.getY(), laneHeaderWidth, rulerHeight).reduced (4, 2);
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

TimelineLayout timelineLayout (juce::Rectangle<int> area, const std::vector<int>& takeLanesPerTrack)
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

    auto y = rest.getY();
    for (const auto takeLanes : takeLanesPerTrack)
    {
        auto row = juce::Rectangle<int> (rest.getX(), y, rest.getWidth(), trackLaneHeight);
        y += trackLaneHeight;
        layout.trackHeaders.push_back (row.removeFromLeft (laneHeaderWidth));
        layout.trackLanes.push_back (row);

        std::vector<juce::Rectangle<int>> headers;
        std::vector<juce::Rectangle<int>> lanes;
        for (int i = 0; i < takeLanes; ++i)
        {
            auto laneRow = juce::Rectangle<int> (rest.getX(), y, rest.getWidth(), takeLaneHeight);
            y += takeLaneHeight;
            headers.push_back (laneRow.removeFromLeft (laneHeaderWidth));
            lanes.push_back (laneRow);
        }
        layout.takeLaneHeaders.push_back (std::move (headers));
        layout.takeLanes.push_back (std::move (lanes));
    }
    return layout;
}

TrackHeaderLayout trackHeaderLayout (juce::Rectangle<int> header)
{
    TrackHeaderLayout layout;
    auto area = header.reduced (6, 6);
    auto top = area.removeFromTop (28);
    layout.arm = top.removeFromLeft (40).withSizeKeepingCentre (40, 24);
    top.removeFromLeft (gap);
    layout.name = top;
    area.removeFromTop (rowGap);
    auto bottom = area.removeFromTop (22);
    layout.solo = bottom.removeFromRight (22);
    bottom.removeFromRight (2);
    layout.mute = bottom.removeFromRight (22);
    bottom.removeFromRight (gap);
    layout.takesToggle = bottom.removeFromLeft (std::min (96, bottom.getWidth()));
    return layout;
}

MixerLayout mixerLayout (juce::Rectangle<int> mixer, int numTracks)
{
    MixerLayout layout;
    auto area = mixer.reduced (gap);
    layout.master = area.removeFromRight (mixerStripWidth);
    area.removeFromRight (2 * margin);
    for (int i = 0; i < numTracks; ++i)
    {
        if (area.getWidth() < mixerStripWidth)
        {
            layout.strips.emplace_back();
            continue;
        }
        layout.strips.push_back (area.removeFromLeft (mixerStripWidth));
        area.removeFromLeft (gap);
    }
    return layout;
}

MixerStripLayout mixerStripLayout (juce::Rectangle<int> strip)
{
    MixerStripLayout layout;
    auto area = strip.reduced (6);
    layout.name = area.removeFromTop (18);
    area.removeFromTop (rowGap);
    layout.pan = area.removeFromTop (18);
    area.removeFromTop (rowGap);
    auto buttons = area.removeFromBottom (22);
    area.removeFromBottom (rowGap);
    layout.value = area.removeFromBottom (16);
    area.removeFromBottom (rowGap);
    // The fader and meter, side by side, centred.
    auto faderRow = area.withSizeKeepingCentre (24 + gap + 10, area.getHeight());
    layout.fader = faderRow.removeFromLeft (24);
    faderRow.removeFromLeft (gap);
    layout.meter = faderRow;
    const auto buttonWidth = (buttons.getWidth() - gap) / 2;
    layout.mute = buttons.removeFromLeft (buttonWidth);
    layout.solo = buttons.removeFromRight (buttonWidth);
    return layout;
}

juce::Rectangle<int> passesPanelBounds (juce::Rectangle<int> page, juce::Rectangle<int> passesButton, int numRows)
{
    const auto wanted = juce::Rectangle<int> (passesButton.getX(), passesButton.getBottom() + 2, passesPanelWidth, passesPanelHeaderHeight + numRows * passRowHeight + 6);
    return wanted.constrainedWithin (page).getIntersection (page);
}

PassesPanelLayout passesPanelLayout (juce::Rectangle<int> panel, int numRows)
{
    PassesPanelLayout layout;
    auto area = panel.reduced (6, 0);
    layout.heading = area.removeFromTop (passesPanelHeaderHeight);
    for (int i = 0; i < numRows; ++i)
        layout.rows.push_back (area.removeFromTop (passRowHeight));
    return layout;
}

PassRowLayout passRowLayout (juce::Rectangle<int> row)
{
    PassRowLayout layout;
    auto area = row.reduced (0, 3);
    layout.bringBack = area.removeFromRight (110);
    area.removeFromRight (gap);
    layout.status = area.removeFromRight (90);
    area.removeFromRight (gap);
    layout.title = area;
    return layout;
}
} // namespace overdub::ui
