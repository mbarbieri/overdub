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
constexpr int controlIndicatorWidth = 130;
constexpr int exportSlotWidth = 150;
constexpr int transposeFieldWidth = 30;
constexpr int practiceSpeedFieldWidth = 40;
constexpr int fieldUnitWidth = 12;
constexpr int fieldSpinnerWidth = 14;
constexpr int transposeSlotWidth = transposeFieldWidth + 2 + fieldUnitWidth + 2 + fieldSpinnerWidth;
// Wider than its field row: "PRACTICE SPEED" needs it.
constexpr int practiceSpeedSlotWidth = 74;
constexpr int fieldCaptionHeight = 12;

// A captioned field's parts in `slot`: the caption over the field, its unit and its spinner.
struct FieldSlot
{
    juce::Rectangle<int> caption;
    juce::Rectangle<int> field;
    juce::Rectangle<int> unit;
    juce::Rectangle<int> spinner;
};

FieldSlot fieldSlot (juce::Rectangle<int> slot, int fieldWidth)
{
    FieldSlot parts;
    parts.caption = slot.removeFromTop (fieldCaptionHeight);
    parts.field = slot.removeFromLeft (fieldWidth);
    slot.removeFromLeft (2);
    parts.unit = slot.removeFromLeft (fieldUnitWidth);
    slot.removeFromLeft (2);
    parts.spinner = slot.removeFromLeft (fieldSpinnerWidth).withSizeKeepingCentre (fieldSpinnerWidth, fieldSpinnerWidth);
    return parts;
}
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
    area.removeFromLeft (gap);
    layout.controlIndicator = area.removeFromLeft (controlIndicatorWidth);
    area.removeFromLeft (gap);

    layout.exportSlot = area.removeFromRight (exportSlotWidth);
    area.removeFromRight (gap);
    layout.transpose = area.removeFromRight (transposeSlotWidth);
    area.removeFromRight (gap);
    const auto transpose = fieldSlot (layout.transpose, transposeFieldWidth);
    layout.transposeCaption = transpose.caption;
    layout.transposeField = transpose.field;
    layout.transposeUnit = transpose.unit;
    layout.transposeSpinner = transpose.spinner;
    layout.practiceSpeed = area.removeFromRight (practiceSpeedSlotWidth);
    area.removeFromRight (gap);
    const auto practiceSpeed = fieldSlot (layout.practiceSpeed, practiceSpeedFieldWidth);
    layout.practiceSpeedCaption = practiceSpeed.caption;
    layout.practiceSpeedField = practiceSpeed.field;
    layout.practiceSpeedUnit = practiceSpeed.unit;
    layout.practiceSpeedSpinner = practiceSpeed.spinner;

    const auto groupWidth = 3 * transportButtonWidth + 3 * gap + clockWidth;
    const auto undoWidth = std::min (undoButtonWidth, (area.getWidth() - groupWidth - 2 * gap) / 2);
    layout.redo = area.removeFromRight (undoWidth);
    area.removeFromRight (gap);
    layout.undo = area.removeFromRight (undoWidth);

    // The transport group sits in the middle of what is left.
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
    layout.cameraPicker = stageBar.removeFromRight (std::min (220, std::max (0, stageBar.getWidth() - 100 - 2 * gap)));
    stageBar.removeFromRight (gap);
    layout.cameraArm = stageBar.removeFromRight (100);
    area.removeFromTop (gap);
    layout.stage = area;

    for (int i = 0; i < numTracks; ++i)
    {
        layout.rows.push_back (column.removeFromTop (trackRowHeight));
        column.removeFromTop (rowGap);
    }
    column.removeFromTop (gap);
    auto buttons = column.removeFromTop (importButtonHeight);
    layout.addTrackButton = buttons.removeFromLeft (120);
    buttons.removeFromLeft (gap);
    layout.importButton = buttons.removeFromLeft (120);
    column.removeFromTop (margin);
    layout.passes = column;
    return layout;
}

TrackRowLayout trackRowLayout (juce::Rectangle<int> row)
{
    TrackRowLayout layout;
    auto area = row.reduced (6, 6);
    // The grip's column on the left; everything else lines up with the name.
    const auto gripColumn = area.removeFromLeft (trackGripWidth);
    area.removeFromLeft (2);
    const auto meterLine = area.removeFromBottom (trackRowClipSize);
    area.removeFromBottom (2);
    auto volumeLine = area.removeFromBottom (trackRowFaderHeight);
    layout.volumeText = volumeLine.removeFromRight (trackRowVolumeTextWidth);
    volumeLine.removeFromRight (gap);
    layout.volumeFader = volumeLine;
    const auto travelInset = faderThumbSize / 2;
    layout.volumeMeter = { volumeLine.getX() + travelInset, meterLine.getCentreY() - trackRowMeterHeight / 2, volumeLine.getWidth() - 2 * travelInset, trackRowMeterHeight };
    layout.clipIndicator = { layout.volumeMeter.getRight() + 2, meterLine.getY(), trackRowClipSize, trackRowClipSize };
    area.removeFromBottom (4);
    layout.insertChain = area.removeFromBottom (insertChipHeight);
    area.removeFromBottom (4);
    layout.monitoring = area.removeFromRight (116);
    area.removeFromRight (gap);
    layout.input = area.removeFromRight (120);
    area.removeFromRight (gap);
    layout.solo = area.removeFromRight (26);
    area.removeFromRight (2);
    layout.mute = area.removeFromRight (26);
    area.removeFromRight (2);
    layout.arm = area.removeFromRight (26);
    area.removeFromRight (gap);
    layout.name = area;
    layout.grip = gripColumn.withY (area.getY()).withHeight (area.getHeight());
    return layout;
}

InsertChainLayout insertChainLayout (juce::Rectangle<int> area, int numChips, bool vertical)
{
    InsertChainLayout layout;
    auto rest = area;
    if (vertical)
        layout.addButton = rest.removeFromBottom (insertChipHeight);
    else
        layout.addButton = rest.removeFromLeft (insertAddButtonWidth);
    for (int i = 0; i < numChips; ++i)
    {
        if (vertical)
        {
            // A chip needs its height plus the gap that keeps it off "+ Insert".
            if (rest.getHeight() < insertChipHeight + insertChipGap)
            {
                layout.chips.emplace_back();
                continue;
            }
            layout.chips.push_back (rest.removeFromTop (insertChipHeight));
            rest.removeFromTop (insertChipGap);
            continue;
        }
        if (rest.getWidth() < insertChipGap * 2 + insertChipWidth)
        {
            layout.chips.emplace_back();
            continue;
        }
        rest.removeFromLeft (insertChipGap * 2);
        layout.chips.push_back (rest.removeFromLeft (insertChipWidth));
    }
    return layout;
}

InsertChipLayout insertChipLayout (juce::Rectangle<int> chip)
{
    InsertChipLayout layout;
    auto area = chip;
    layout.remove = area.removeFromRight (16);
    layout.bypass = area.removeFromRight (16);
    layout.name = area;
    return layout;
}

std::size_t insertDropIndex (const std::vector<juce::Rectangle<int>>& chips, std::size_t dragged, juce::Point<int> point, bool vertical)
{
    std::size_t index = 0;
    for (std::size_t i = 0; i < chips.size(); ++i)
    {
        if (i == dragged || chips[i].isEmpty())
            continue;
        const auto centre = chips[i].getCentre();
        if (vertical ? centre.y < point.y : centre.x < point.x)
            ++index;
    }
    return index;
}

std::size_t trackDropIndex (const std::vector<juce::Range<int>>& rows, std::size_t dragged, int y)
{
    std::size_t index = 0;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (i != dragged && (rows[i].getStart() + rows[i].getEnd()) / 2 < y)
            ++index;
    return index;
}

int trackDropLineY (const std::vector<juce::Range<int>>& rows, std::size_t dragged, std::size_t dropIndex)
{
    std::vector<juce::Range<int>> others;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (i != dragged)
            others.push_back (rows[i]);
    if (others.empty())
        return dragged < rows.size() ? rows[dragged].getStart() : 0;
    if (dropIndex == 0)
        return others.front().getStart();
    if (dropIndex >= others.size())
        return others.back().getEnd();
    return (others[dropIndex - 1].getEnd() + others[dropIndex].getStart()) / 2;
}

int clampMixerHeight (int requested, int pageHeight)
{
    const auto largest = pageHeight - clipToolbarHeight - mixerDividerHeight - minimumTimelineHeight;
    return std::max (minimumMixerHeight, std::min (requested, largest));
}

EditPageLayout editPageLayout (juce::Rectangle<int> page, bool mixerVisible, int mixerHeight, bool cameraArmed)
{
    EditPageLayout layout;
    auto area = page;
    layout.clipToolbar = area.removeFromTop (clipToolbarHeight);
    if (mixerVisible)
    {
        layout.mixer = area.removeFromBottom (clampMixerHeight (mixerHeight, page.getHeight()));
        layout.mixerDivider = area.removeFromBottom (mixerDividerHeight);
        if (cameraArmed)
            layout.videoPreview = layout.mixer.removeFromRight (videoPreviewWidth).reduced (gap);
    }
    layout.timeline = area;
    layout.passesButton = juce::Rectangle<int> (area.getX(), area.getY(), laneHeaderWidth, rulerHeight).reduced (4, 2);
    return layout;
}

ClipToolbarLayout clipToolbarLayout (juce::Rectangle<int> bar)
{
    ClipToolbarLayout layout;
    auto area = bar.reduced (margin, 5);
    layout.mixerToggle = area.removeFromRight (64);
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

TimelineLayout timelineLayout (juce::Rectangle<int> area, const std::vector<int>& takeLanesPerTrack, int verticalScroll)
{
    TimelineLayout layout;
    for (const auto takeLanes : takeLanesPerTrack)
    {
        const auto rowHeight = trackLaneHeight + takeLanes * takeLaneHeight;
        layout.trackRows.push_back (juce::Range<int>::withStartAndLength (layout.tracksHeight, rowHeight));
        layout.tracksHeight += rowHeight;
    }

    auto rest = area;
    auto scrollRow = rest.removeFromBottom (timelineScrollBarHeight);
    auto rulerRow = rest.removeFromTop (rulerHeight);
    auto cameraRow = rest.removeFromTop (cameraLaneHeight);
    if (layout.tracksHeight > rest.getHeight())
    {
        layout.verticalScrollBar = rest.removeFromRight (timelineScrollBarHeight);
        rulerRow.removeFromRight (timelineScrollBarHeight);
        cameraRow.removeFromRight (timelineScrollBarHeight);
        scrollRow.removeFromRight (timelineScrollBarHeight);
    }
    layout.tracksViewport = rest;

    scrollRow.removeFromLeft (laneHeaderWidth);
    layout.scrollBar = scrollRow;
    layout.rulerCorner = rulerRow.removeFromLeft (laneHeaderWidth);
    layout.ruler = rulerRow;
    layout.cameraHeader = cameraRow.removeFromLeft (laneHeaderWidth);
    const auto trackArm = trackHeaderLayout (layout.cameraHeader.withHeight (trackLaneHeight)).arm;
    layout.cameraArm = trackArm.withCentre ({ trackArm.getCentreX(), layout.cameraHeader.getCentreY() });
    layout.cameraLane = cameraRow;

    auto y = rest.getY() - verticalScroll;
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

int clampTimelineScroll (int verticalScroll, int tracksHeight, int viewportHeight)
{
    return std::clamp (verticalScroll, 0, std::max (0, tracksHeight - viewportHeight));
}

int revealTimelineScroll (int verticalScroll, int spanTop, int spanBottom, int viewportHeight)
{
    auto scroll = verticalScroll;
    if (spanBottom - scroll > viewportHeight)
        scroll = spanBottom - viewportHeight;
    if (spanTop < scroll)
        scroll = spanTop;
    return scroll;
}

TrackHeaderLayout trackHeaderLayout (juce::Rectangle<int> header)
{
    TrackHeaderLayout layout;
    auto area = header.reduced (6, 6);
    layout.name = area.removeFromTop (28);
    layout.grip = layout.name.removeFromLeft (trackGripWidth);
    layout.name.removeFromLeft (2);
    area.removeFromTop (rowGap);
    auto bottom = area.removeFromTop (22);
    layout.deleteButton = bottom.removeFromRight (22);
    bottom.removeFromRight (gap);
    layout.solo = bottom.removeFromRight (22);
    bottom.removeFromRight (2);
    layout.mute = bottom.removeFromRight (22);
    bottom.removeFromRight (2);
    layout.arm = bottom.removeFromRight (22);
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
    layout.inserts = area.removeFromTop (mixerInsertsHeight);
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
