#include "ui/Layout.h"

#include "ui/Colours.h"

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

    checkInsideAndApart (bar, { layout.modeButtons[0], layout.modeButtons[1], layout.modeButtons[2], layout.controlIndicator, layout.rewind, layout.playStop, layout.record, layout.clock, layout.undo, layout.redo, layout.practiceSpeed, layout.transpose, layout.exportSlot });
    CHECK (layout.modeButtons[0].getX() < layout.modeButtons[1].getX());
    CHECK (layout.modeButtons[1].getX() < layout.modeButtons[2].getX());
    // Record · Edit · Transcribe (issue #225): Transcribe is the widest label.
    CHECK (layout.modeButtons[2].getWidth() == 80);
    CHECK (layout.modeButtons[2].getRight() < layout.controlIndicator.getX());
    CHECK (layout.controlIndicator.getRight() < layout.rewind.getX());
    CHECK (layout.controlIndicator.getWidth() == 130);
    CHECK (layout.undo.getWidth() == 49);
    CHECK (layout.clock.getRight() < layout.undo.getX());
    // Transpose between Redo and Export... (issue #209): its caption over the field, "st" and the
    // spinner.
    CHECK (layout.practiceSpeed.getRight() < layout.transpose.getX());
    CHECK (layout.transpose.getWidth() == 60);
    CHECK (layout.transposeCaption.getBottom() <= layout.transposeField.getY());
    CHECK (layout.transposeField.getRight() < layout.transposeUnit.getX());
    CHECK (layout.transposeUnit.getRight() <= layout.transposeSpinner.getX());
    CHECK (layout.transpose.contains (layout.transposeSpinner));
    // Practice Speed between Redo and Transpose (issue #212), laid out the same way.
    CHECK (layout.redo.getRight() < layout.practiceSpeed.getX());
    CHECK (layout.practiceSpeedCaption.getWidth() == layout.practiceSpeed.getWidth());
    CHECK (layout.practiceSpeedCaption.getBottom() <= layout.practiceSpeedField.getY());
    CHECK (layout.practiceSpeedField.getWidth() == 40);
    CHECK (layout.practiceSpeedField.getRight() < layout.practiceSpeedUnit.getX());
    CHECK (layout.practiceSpeedUnit.getRight() <= layout.practiceSpeedSpinner.getX());
    CHECK (layout.practiceSpeed.contains (layout.practiceSpeedSpinner));
    // Export... at the right end (issue #200).
    CHECK (layout.transpose.getRight() < layout.exportSlot.getX());
    CHECK (layout.exportSlot.getWidth() == 150);
    CHECK (layout.exportSlot.getRight() <= bar.getRight());
}

TEST_CASE ("at the fixture width Undo and Redo are 129 wide beside the control indicator and the Practice Speed field", "[ui][layout]")
{
    const Rectangle<int> bar { 0, 0, 1200, topBarHeight };
    const auto layout = topBarLayout (bar);

    checkInsideAndApart (bar, { layout.modeButtons[2], layout.controlIndicator, layout.rewind, layout.clock, layout.undo, layout.redo, layout.practiceSpeed, layout.transpose, layout.exportSlot });
    CHECK (layout.controlIndicator.getRight() < layout.rewind.getX());
    CHECK (layout.undo.getWidth() == 129);
    CHECK (layout.redo.getWidth() == 129);
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

TEST_CASE ("a Track row puts name, A, M, S, input picker and Monitoring left to right", "[ui][layout][arm]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.mute, layout.solo, layout.input, layout.monitoring });
    CHECK (layout.name.getRight() < layout.arm.getX());
    CHECK (layout.arm.getY() == layout.mute.getY());
    CHECK (layout.arm.getRight() < layout.mute.getX());
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

TEST_CASE ("a Track row puts its volume fader along the bottom, under the Insert Chain, with the dB text at its right", "[ui][layout][record_fader]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.mute, layout.solo, layout.input, layout.monitoring, layout.insertChain, layout.volumeFader, layout.volumeText });
    CHECK (layout.volumeFader.getY() >= layout.insertChain.getBottom());
    CHECK (layout.volumeFader.getX() == layout.name.getX());
    CHECK (layout.volumeFader.getHeight() == trackRowFaderHeight);
    CHECK (layout.volumeText.getX() > layout.volumeFader.getRight());
    CHECK (layout.volumeText.getRight() == layout.monitoring.getRight());
    CHECK (layout.volumeText.getWidth() == trackRowVolumeTextWidth);
    // Room to drag: a pixel is well under a dB below 0 dB.
    CHECK (layout.volumeFader.getWidth() >= 300);
}

TEST_CASE ("a Track row puts its level meter under the fader along the thumb's travel, and the clip dot at the meter's right end", "[ui][layout][record_meter]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto layout = trackRowLayout (row);

    checkInsideAndApart (row, { layout.arm, layout.name, layout.mute, layout.solo, layout.input, layout.monitoring, layout.insertChain, layout.volumeFader, layout.volumeText, layout.volumeMeter, layout.clipIndicator });
    CHECK (layout.volumeMeter.getY() >= layout.volumeFader.getBottom());
    CHECK (layout.volumeMeter.getHeight() == trackRowMeterHeight);
    // The thumb's centre travels from half a thumb in from the fader's left to half a thumb in from its right.
    CHECK (layout.volumeMeter.getX() == layout.volumeFader.getX() + faderThumbSize / 2);
    CHECK (layout.volumeMeter.getRight() == layout.volumeFader.getRight() - faderThumbSize / 2);
    CHECK (layout.clipIndicator.getX() > layout.volumeMeter.getRight());
    CHECK (layout.clipIndicator.getCentreY() == layout.volumeMeter.getCentreY());
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

TEST_CASE ("a Track row and an Edit Track header put the grip left of the name, right of the colour stripe", "[ui][layout][track_reorder]")
{
    const Rectangle<int> row { 800, 100, trackColumnWidth, trackRowHeight };
    const auto rowLayout = trackRowLayout (row);
    checkInsideAndApart (row, { rowLayout.grip, rowLayout.name, rowLayout.arm, rowLayout.mute, rowLayout.solo, rowLayout.input, rowLayout.monitoring, rowLayout.insertChain });
    CHECK (rowLayout.grip.getX() >= row.getX() + trackColourStripeWidth + 1);
    CHECK (rowLayout.grip.getWidth() == trackGripWidth);
    CHECK (rowLayout.grip.getY() == rowLayout.name.getY());
    CHECK (rowLayout.grip.getHeight() == rowLayout.name.getHeight());
    CHECK (rowLayout.grip.getRight() < rowLayout.name.getX());

    const Rectangle<int> header { 0, 100, laneHeaderWidth, trackLaneHeight };
    const auto headerLayout = trackHeaderLayout (header);
    checkInsideAndApart (header, { headerLayout.grip, headerLayout.name, headerLayout.arm, headerLayout.takesToggle, headerLayout.mute, headerLayout.solo, headerLayout.deleteButton });
    CHECK (headerLayout.grip.getX() >= header.getX() + trackColourStripeWidth + 1);
    CHECK (headerLayout.grip.getWidth() == trackGripWidth);
    CHECK (headerLayout.grip.getY() == headerLayout.name.getY());
    CHECK (headerLayout.grip.getRight() < headerLayout.name.getX());
}

TEST_CASE ("a dragged Track lands after every other row whose centre is above the pointer", "[ui][layout][track_reorder]")
{
    // Edit rows from y 100, the middle one with its Take Lanes unfolded: centres 138, 236, 334.
    const std::vector<juce::Range<int>> rows { { 100, 176 }, { 176, 296 }, { 296, 372 } };

    // The first row dragged: past the second's centre it goes second, past the third's last.
    CHECK (trackDropIndex (rows, 0, 236) == 0);
    CHECK (trackDropIndex (rows, 0, 237) == 1);
    CHECK (trackDropIndex (rows, 0, 334) == 1);
    CHECK (trackDropIndex (rows, 0, 335) == 2);
    CHECK (trackDropIndex (rows, 0, 5000) == 2);
    // The last row dragged above the first's centre goes first.
    CHECK (trackDropIndex (rows, 2, 138) == 0);
    CHECK (trackDropIndex (rows, 2, 139) == 1);
    CHECK (trackDropIndex (rows, 2, -40) == 0);
    // Anywhere within its own row, between its neighbours' centres, it stays.
    CHECK (trackDropIndex (rows, 1, 139) == 1);
    CHECK (trackDropIndex (rows, 1, 334) == 1);
    // The dragged row's own centre does not count.
    CHECK (trackDropIndex (rows, 1, 300) == 1);
}

TEST_CASE ("the insertion line sits between the other rows the drop falls between", "[ui][layout][track_reorder]")
{
    // Record rows: 98 tall with a 4 point gap.
    const std::vector<juce::Range<int>> rows { { 10, 108 }, { 112, 210 }, { 214, 312 } };

    // Dragging the first: before row 2, between rows 2 and 3, after row 3.
    CHECK (trackDropLineY (rows, 0, 0) == 112);
    CHECK (trackDropLineY (rows, 0, 1) == (210 + 214) / 2);
    CHECK (trackDropLineY (rows, 0, 2) == 312);
    // Dragging the middle one: before row 1, between rows 1 and 3, after row 3.
    CHECK (trackDropLineY (rows, 1, 0) == 10);
    CHECK (trackDropLineY (rows, 1, 1) == (108 + 214) / 2);
    CHECK (trackDropLineY (rows, 1, 2) == 312);
    // The only Track: its own top.
    CHECK (trackDropLineY ({ { 10, 108 } }, 0, 0) == 10);
}

TEST_CASE ("Edit mode stacks the Clip toolbar, the timeline, the divider and the Mixer region", "[ui][layout][edit]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = editPageLayout (page, true, 220, false);

    checkInsideAndApart (page, { layout.clipToolbar, layout.timeline, layout.mixerDivider, layout.mixer });
    CHECK (layout.videoPreview.isEmpty());
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
    const auto layout = editPageLayout (page, false, 220, false);

    CHECK (layout.mixer.isEmpty());
    CHECK (layout.mixerDivider.isEmpty());
    CHECK (layout.timeline.getBottom() == page.getBottom());
}

TEST_CASE ("the Video Preview takes the Mixer region's right end, a fixed width at its full height", "[ui][layout][edit][video_preview]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = editPageLayout (page, true, 300, true);

    checkInsideAndApart (page, { layout.clipToolbar, layout.timeline, layout.mixerDivider, layout.mixer, layout.videoPreview });
    // videoPreviewWidth at the region's full height, inset by 6 px as the Mixer's strips are.
    CHECK (layout.videoPreview.getWidth() == videoPreviewWidth - 12);
    CHECK (layout.videoPreview.getRight() == page.getRight() - 6);
    CHECK (layout.videoPreview.getY() == layout.mixerDivider.getBottom() + 6);
    CHECK (layout.videoPreview.getBottom() == page.getBottom() - 6);
    CHECK (layout.mixer.getX() == page.getX());
    CHECK (layout.mixer.getRight() == layout.videoPreview.getX() - 6);
    CHECK (layout.mixer.getHeight() == 300);
    // The divider still spans the page, over the Mixer and the Video Preview.
    CHECK (layout.mixerDivider.getWidth() == page.getWidth());
}

TEST_CASE ("a hidden Mixer region hides the Video Preview with it", "[ui][layout][edit][video_preview]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto layout = editPageLayout (page, false, 300, true);

    CHECK (layout.videoPreview.isEmpty());
    CHECK (layout.mixer.isEmpty());
    CHECK (layout.timeline.getBottom() == page.getBottom());
}

TEST_CASE ("the Mixer region's height stays between its minimum and what leaves the timeline room", "[ui][layout][edit]")
{
    const int pageHeight = 700;
    CHECK (clampMixerHeight (10, pageHeight) == minimumMixerHeight);
    CHECK (clampMixerHeight (300, pageHeight) == 300);
    CHECK (clampMixerHeight (5000, pageHeight) == pageHeight - clipToolbarHeight - mixerDividerHeight - minimumTimelineHeight);

    const Rectangle<int> page { 0, 0, 1200, pageHeight };
    CHECK (editPageLayout (page, true, 5000, false).timeline.getHeight() == minimumTimelineHeight);
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

TEST_CASE ("the Camera lane header puts its A where a Track header puts its A, centred on the lane", "[ui][layout][edit][arm]")
{
    const Rectangle<int> area { 0, 88, 1200, 500 };
    const auto layout = timelineLayout (area, { 0 });
    const auto trackArm = trackHeaderLayout (layout.trackHeaders[0]).arm;

    CHECK (layout.cameraHeader.contains (layout.cameraArm));
    CHECK (layout.cameraArm.getX() == trackArm.getX());
    CHECK (layout.cameraArm.getWidth() == trackArm.getWidth());
    CHECK (layout.cameraArm.getHeight() == trackArm.getHeight());
    CHECK (layout.cameraArm.getCentreY() == layout.cameraHeader.getCentreY());
}

TEST_CASE ("a Track header puts the name on top, and A, M, S and × right of the Takes toggle under it", "[ui][layout][edit][arm]")
{
    const Rectangle<int> header { 0, 100, laneHeaderWidth, trackLaneHeight };
    const auto layout = trackHeaderLayout (header);
    checkInsideAndApart (header, { layout.arm, layout.name, layout.takesToggle, layout.mute, layout.solo, layout.deleteButton });
    CHECK (layout.takesToggle.getY() >= layout.name.getBottom());
    CHECK (layout.arm.getY() == layout.takesToggle.getY());
    CHECK (layout.mute.getY() == layout.takesToggle.getY());
    CHECK (layout.takesToggle.getRight() < layout.arm.getX());
    CHECK (layout.arm.getRight() < layout.mute.getX());
    CHECK (layout.mute.getRight() < layout.solo.getX());
    // "▸ 3 Takes" keeps the width it had before A joined the row.
    CHECK (layout.takesToggle.getWidth() >= 58);
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
    const auto edit = editPageLayout (page, true, defaultMixerHeight, false);
    CHECK (edit.timeline.contains (edit.passesButton));
    // The ruler is under the Loop/Marker strip (issue #227).
    CHECK (edit.passesButton.getY() >= edit.timeline.getY() + markerStripHeight);
    CHECK (edit.passesButton.getBottom() <= edit.timeline.getY() + markerStripHeight + rulerHeight);
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

TEST_CASE ("Tracks that fit show no vertical scroll bar and fill the timeline's width", "[ui][layout][edit][vertical_scroll]")
{
    const Rectangle<int> area { 0, 88, 1200, 500 };
    const auto layout = timelineLayout (area, { 0, 0 }, 0);

    CHECK (layout.verticalScrollBar.isEmpty());
    CHECK (layout.tracksHeight == 2 * trackLaneHeight);
    CHECK (layout.tracksViewport.getY() == layout.cameraLane.getBottom());
    CHECK (layout.tracksViewport.getBottom() == layout.scrollBar.getY());
    CHECK (layout.tracksViewport.getX() == area.getX());
    CHECK (layout.tracksViewport.getRight() == area.getRight());
    CHECK (layout.ruler.getRight() == area.getRight());
}

TEST_CASE ("Tracks that overflow get a vertical scroll bar beside them, and the ruler and lanes end left of it", "[ui][layout][edit][vertical_scroll]")
{
    const Rectangle<int> area { 0, 88, 1200, 300 };
    const auto layout = timelineLayout (area, { 2, 0, 0, 0 }, 0);

    REQUIRE (! layout.verticalScrollBar.isEmpty());
    CHECK (layout.tracksHeight == 4 * trackLaneHeight + 2 * takeLaneHeight);
    CHECK (layout.verticalScrollBar.getRight() == area.getRight());
    CHECK (layout.verticalScrollBar.getWidth() == timelineScrollBarHeight);
    CHECK (layout.verticalScrollBar.getY() == layout.cameraLane.getBottom());
    CHECK (layout.verticalScrollBar.getBottom() == layout.scrollBar.getY());
    for (const auto& part : { layout.ruler, layout.cameraLane, layout.trackLanes[0], layout.takeLanes[0][0], layout.scrollBar, layout.tracksViewport })
        CHECK (part.getRight() == layout.verticalScrollBar.getX());
}

TEST_CASE ("a vertical scroll moves Track headers, lanes and Take Lanes up, and leaves the ruler, Camera lane and scroll bar in place", "[ui][layout][edit][vertical_scroll]")
{
    const Rectangle<int> area { 0, 88, 1200, 300 };
    const auto still = timelineLayout (area, { 2, 0, 0, 0 }, 0);
    const auto scrolled = timelineLayout (area, { 2, 0, 0, 0 }, 50);

    CHECK (scrolled.ruler == still.ruler);
    CHECK (scrolled.cameraLane == still.cameraLane);
    CHECK (scrolled.scrollBar == still.scrollBar);
    CHECK (scrolled.verticalScrollBar == still.verticalScrollBar);
    CHECK (scrolled.tracksViewport == still.tracksViewport);
    for (std::size_t i = 0; i < 4; ++i)
    {
        CHECK (scrolled.trackLanes[i] == still.trackLanes[i].translated (0, -50));
        CHECK (scrolled.trackHeaders[i] == still.trackHeaders[i].translated (0, -50));
    }
    CHECK (scrolled.takeLanes[0][1] == still.takeLanes[0][1].translated (0, -50));
    CHECK (scrolled.takeLaneHeaders[0][1] == still.takeLaneHeaders[0][1].translated (0, -50));
}

TEST_CASE ("each Track's row spans its lane and its Take Lanes, measured from the first Track's top whatever the scroll", "[ui][layout][edit][vertical_scroll]")
{
    const Rectangle<int> area { 0, 88, 1200, 300 };
    const auto layout = timelineLayout (area, { 2, 0, 1 }, 70);

    REQUIRE (layout.trackRows.size() == 3);
    CHECK (layout.trackRows[0] == juce::Range<int> (0, trackLaneHeight + 2 * takeLaneHeight));
    CHECK (layout.trackRows[1] == juce::Range<int>::withStartAndLength (layout.trackRows[0].getEnd(), trackLaneHeight));
    CHECK (layout.trackRows[2] == juce::Range<int>::withStartAndLength (layout.trackRows[1].getEnd(), trackLaneHeight + takeLaneHeight));
    CHECK (layout.trackRows[2].getEnd() == layout.tracksHeight);
    CHECK (layout.trackLanes[1].getY() == layout.tracksViewport.getY() - 70 + layout.trackRows[1].getStart());
}

TEST_CASE ("the vertical scroll stays between the top and where the last Track's bottom meets the lanes' bottom", "[ui][layout][edit][vertical_scroll]")
{
    CHECK (clampTimelineScroll (-20, 500, 200) == 0);
    CHECK (clampTimelineScroll (120, 500, 200) == 120);
    CHECK (clampTimelineScroll (400, 500, 200) == 300);
    CHECK (clampTimelineScroll (80, 150, 200) == 0); // everything fits
}

TEST_CASE ("revealing Tracks scrolls just enough to bring them into view, their top first when they are taller than it", "[ui][layout][edit][vertical_scroll]")
{
    // Viewport 200 high, scrolled to 0. Already visible: unchanged.
    CHECK (revealTimelineScroll (0, 50, 150, 200) == 0);
    // Below the bottom: scrolled so their bottom meets the viewport's bottom.
    CHECK (revealTimelineScroll (0, 300, 400, 200) == 200);
    // Above the top: scrolled so their top meets the viewport's top.
    CHECK (revealTimelineScroll (250, 100, 180, 200) == 100);
    // Taller than the viewport: their top.
    CHECK (revealTimelineScroll (0, 300, 700, 200) == 300);
}

TEST_CASE ("Transcribe mode stacks the overview over the ruler and the waveform, beside the column while it shows", "[ui][layout][transcribe]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };
    const auto shown = transcribePageLayout (page, true);

    checkInsideAndApart (page, { shown.overview, shown.ruler, shown.waveform, shown.column });
    CHECK (shown.overview.getHeight() == transcribeOverviewHeight);
    CHECK (shown.ruler.getHeight() == transcribeRulerHeight);
    CHECK (shown.overview.getBottom() < shown.ruler.getY());
    CHECK (shown.ruler.getBottom() == shown.waveform.getY());
    CHECK (shown.overview.getX() == shown.waveform.getX());
    CHECK (shown.overview.getWidth() == shown.waveform.getWidth());
    CHECK (shown.waveform.getHeight() > 400);
    CHECK (shown.column.getWidth() == transcribeColumnWidth);
    CHECK (shown.waveform.getRight() < shown.column.getX());
    CHECK (shown.column.getHeight() == page.getHeight() - 20);

    const auto hidden = transcribePageLayout (page, false);
    CHECK (hidden.column.isEmpty());
    CHECK (hidden.waveform.getWidth() == shown.waveform.getWidth() + transcribeColumnWidth + 10);
    CHECK (hidden.overview.getWidth() == hidden.waveform.getWidth());

    const auto column = transcribeColumnLayout (shown.column.withZeroOrigin());
    checkInsideAndApart (shown.column.withZeroOrigin(), { column.heading, column.practiceSpeedField, column.practiceSpeedUnit, column.transposeField, column.transposeUnit });
    CHECK (column.heading.getBottom() < column.practiceSpeedCaption.getY());
    CHECK (column.practiceSpeedCaption.getBottom() <= column.practiceSpeedField.getY());
    CHECK (column.practiceSpeedField.getBottom() < column.transposeCaption.getY());
    CHECK (column.transposeCaption.getBottom() <= column.transposeField.getY());
    // The fine tune (issue #232): its cents field and "ct" after "st", then the spinner.
    checkInsideAndApart (shown.column.withZeroOrigin(), { column.transposeField, column.transposeUnit, column.transposeCentsField, column.transposeCentsUnit, column.transposeSpinner });
    CHECK (column.transposeUnit.getRight() < column.transposeCentsField.getX());
    CHECK (column.transposeCentsField.getY() == column.transposeField.getY());
    CHECK (column.transposeCentsField.getWidth() == column.transposeField.getWidth());
    CHECK (column.transposeCentsField.getRight() < column.transposeCentsUnit.getX());
    CHECK (column.transposeCentsUnit.getRight() <= column.transposeSpinner.getX());
    // The Focus Track's octave shift (issue #233): a caption over −1, 0, +1, +2 and a spinner,
    // between Transpose and the Transcribe Mix.
    checkInsideAndApart (shown.column.withZeroOrigin(), { column.octaveButtons[0], column.octaveButtons[1], column.octaveButtons[2], column.octaveButtons[3], column.octaveSpinner });
    CHECK (column.transposeField.getBottom() < column.octaveCaption.getY());
    CHECK (column.octaveCaption.getBottom() <= column.octaveButtons[0].getY());
    for (std::size_t i = 1; i < column.octaveButtons.size(); ++i)
    {
        CHECK (column.octaveButtons[i].getY() == column.octaveButtons[0].getY());
        CHECK (column.octaveButtons[i - 1].getRight() < column.octaveButtons[i].getX());
    }
    CHECK (column.octaveButtons[3].getRight() <= column.octaveSpinner.getX());
    CHECK (column.octaveButtons[0].getBottom() < column.mixCaption.getY());
    // The Focus Track's band filter (issue #234): a caption over On, the low cut and "Hz", the high
    // cut and "Hz", between the octave shift and the Transcribe Mix.
    checkInsideAndApart (shown.column.withZeroOrigin(), { column.filterToggle, column.filterLowField, column.filterLowUnit, column.filterHighField, column.filterHighUnit });
    CHECK (column.octaveButtons[0].getBottom() < column.filterCaption.getY());
    CHECK (column.filterCaption.getBottom() <= column.filterToggle.getY());
    CHECK (column.filterToggle.getRight() < column.filterLowField.getX());
    CHECK (column.filterLowField.getRight() < column.filterLowUnit.getX());
    CHECK (column.filterLowUnit.getRight() < column.filterHighField.getX());
    CHECK (column.filterHighField.getRight() < column.filterHighUnit.getX());
    CHECK (column.filterLowField.getWidth() == column.filterHighField.getWidth());
    CHECK (column.filterToggle.getBottom() < column.mixCaption.getY());
}

TEST_CASE ("the Loop/Marker strip sits right above the ruler in Edit and Transcribe, and right above the overview in Record", "[ui][layout][markers]")
{
    const Rectangle<int> page { 0, topBarHeight, 1200, 700 };

    const auto timeline = timelineLayout ({ 0, 88, 1200, 500 }, { 0 });
    CHECK (timeline.markerStrip.getY() == 88);
    CHECK (timeline.markerStrip.getHeight() == markerStripHeight);
    CHECK (timeline.markerStrip.getBottom() == timeline.ruler.getY());
    CHECK (timeline.markerStrip.getX() == timeline.ruler.getX());
    CHECK (timeline.markerStrip.getWidth() == timeline.ruler.getWidth());
    CHECK (timeline.markerStripCorner.getRight() == timeline.markerStrip.getX());
    CHECK (timeline.markerStripCorner.getWidth() == laneHeaderWidth);
    // The Passes button stays over the ruler's corner.
    const auto edit = editPageLayout (page, true, defaultMixerHeight, false);
    const auto editTimeline = timelineLayout (edit.timeline, { 0 });
    CHECK (editTimeline.rulerCorner.contains (edit.passesButton));

    const auto record = recordPageLayout (page, 2);
    CHECK (record.markerStrip.getHeight() == markerStripHeight);
    CHECK (record.markerStrip.getBottom() == record.overview.getY());
    CHECK (record.markerStrip.getX() == record.overview.getX());
    CHECK (record.markerStrip.getWidth() == record.overview.getWidth());
    CHECK (record.lookAhead.getBottom() < record.markerStrip.getY());

    const auto transcribe = transcribePageLayout (page, true);
    checkInsideAndApart (page, { transcribe.loops, transcribe.markers, transcribe.overview, transcribe.markerStrip, transcribe.ruler, transcribe.column });
    CHECK (transcribe.markerStrip.getBottom() == transcribe.ruler.getY());
    CHECK (transcribe.markerStrip.getX() == transcribe.ruler.getX());
    CHECK (transcribe.markerStrip.getWidth() == transcribe.ruler.getWidth());
    CHECK (transcribe.overview.getBottom() < transcribe.markerStrip.getY());
    CHECK (transcribe.markers.getWidth() == transcribeMarkersWidth);
    CHECK (transcribe.markers.getX() == page.getX() + 10);
    // The Loop cards (issue #228) above the Markers, sharing the column's height.
    CHECK (transcribe.loops.getX() == transcribe.markers.getX());
    CHECK (transcribe.loops.getWidth() == transcribeMarkersWidth);
    CHECK (transcribe.loops.getY() == page.getY() + 10);
    CHECK (transcribe.loops.getBottom() < transcribe.markers.getY());
    CHECK (transcribe.markers.getBottom() == page.getBottom() - 10);
    CHECK (transcribe.loops.getHeight() + transcribe.markers.getHeight() > page.getHeight() - 40);
    CHECK (transcribe.markers.getRight() < transcribe.overview.getX());
    CHECK (transcribePageLayout (page, false).markers == transcribe.markers);
    CHECK (transcribePageLayout (page, false).loops == transcribe.loops);
}
