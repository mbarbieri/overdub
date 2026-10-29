#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cstddef>
#include <vector>

// The main window's geometry (ADR 0005 UI amendment): pure functions from a bounds rectangle to
// child rectangles, so Components only call setBounds with what these return. Sizes in points.
namespace overdub::ui
{
inline constexpr int minimumWindowWidth = 1040;
inline constexpr int minimumWindowHeight = 600;
inline constexpr int topBarHeight = 52;
inline constexpr int noticeLineHeight = 28;
inline constexpr int trackColumnWidth = 460;
inline constexpr int trackRowHeight = 98;
// The Loop/Marker strip above the ruler in every mode (issue #227, ui/MarkerStrip.h).
inline constexpr int markerStripHeight = 20;

struct MainLayout
{
    juce::Rectangle<int> topBar;
    juce::Rectangle<int> page;       // the current mode's page
    juce::Rectangle<int> noticeLine; // over the bottom of the page, shown only with a Notice
};

MainLayout mainLayout (juce::Rectangle<int> window);

// Left: Record · Edit · Transcribe, then the control indicator. Centre: ⏮ ▶/■ ● and the clock. Right: Undo,
// Redo, then the export slot, which holds the Export... button, or the running export's progress
// (issue #200). The indicator's slot is kept whether or not it shows, so nothing moves when a
// control client connects (issue #177); Undo and Redo narrow below 190 to make room at small
// widths.
struct TopBarLayout
{
    std::array<juce::Rectangle<int>, 3> modeButtons;
    juce::Rectangle<int> controlIndicator;
    juce::Rectangle<int> rewind;
    juce::Rectangle<int> playStop;
    juce::Rectangle<int> record;
    juce::Rectangle<int> clock;
    juce::Rectangle<int> undo;
    juce::Rectangle<int> redo;
    // Transpose (issue #209): the whole slot, and in it the caption over the field, "st" and the
    // spinner.
    juce::Rectangle<int> transpose;
    juce::Rectangle<int> transposeCaption;
    juce::Rectangle<int> transposeField;
    juce::Rectangle<int> transposeUnit;
    juce::Rectangle<int> transposeSpinner;
    // Practice Speed (issue #212), before Transpose, laid out the same way with "%".
    juce::Rectangle<int> practiceSpeed;
    juce::Rectangle<int> practiceSpeedCaption;
    juce::Rectangle<int> practiceSpeedField;
    juce::Rectangle<int> practiceSpeedUnit;
    juce::Rectangle<int> practiceSpeedSpinner;
    juce::Rectangle<int> exportSlot;
};

TopBarLayout topBarLayout (juce::Rectangle<int> bar);

// Record mode (issues #27, #32, #35, issue #9 resolution, variant C): on the left the stage bar
// (the pre-roll picker after its caption at its left end; the camera arm, then the camera picker,
// at its right end) over the camera stage; in a right column of trackColumnWidth the Track rows,
// the Add Track and Import buttons side by side under the last row, then the Passes list filling
// the rest. Along the bottom, full width, the Backing Track look-ahead strip and under it the
// whole-Song overview, with the read-only Loop/Marker strip (issue #227) right above it.
inline constexpr int stageBarHeight = 30;
inline constexpr int lookAheadStripHeight = 124;
inline constexpr int songOverviewHeight = 40;

struct RecordPageLayout
{
    juce::Rectangle<int> preRollCaption;
    juce::Rectangle<int> preRollPicker;
    juce::Rectangle<int> cameraArm;
    juce::Rectangle<int> cameraPicker;
    juce::Rectangle<int> stage;
    std::vector<juce::Rectangle<int>> rows;
    juce::Rectangle<int> addTrackButton;
    juce::Rectangle<int> importButton;
    juce::Rectangle<int> passes;
    juce::Rectangle<int> lookAhead;
    juce::Rectangle<int> markerStrip;
    juce::Rectangle<int> overview;
};

RecordPageLayout recordPageLayout (juce::Rectangle<int> page, int numTracks);

// The grip (issue #138) on the left, level with the name; everything else lines up with the name.
// Top line, left to right: name, A (arm), M, S, input picker, Monitoring (with its count of
// active Inserts). Middle line, under the name: the Track's Insert Chain (issue #34), laid out by
// insertChainLayout (horizontal). Next line, from the name's left edge: the horizontal volume
// fader (issue #130), then its dB text at the right. Bottom line: the fader's level meter (issue
// #131), spanning the thumb's travel so a peak lines up with the thumb at the same dB, and the clip
// indicator at the meter's right end.
inline constexpr int trackRowFaderHeight = 18;
inline constexpr int trackRowVolumeTextWidth = 56;
inline constexpr int trackRowMeterHeight = 6;
inline constexpr int trackRowClipSize = 8;
// A fader's thumb length along its travel (ui/Mixer.h MixerFader): the travel is the fader's length
// less one thumb, so its ends sit half a thumb in from the fader's.
inline constexpr int faderThumbSize = 10;
// The grip (⋮⋮) that drags a Track to reorder it (issue #138), at the left of an Edit Track
// header's and a Record row's name.
inline constexpr int trackGripWidth = 14;

struct TrackRowLayout
{
    juce::Rectangle<int> insertChain;
    juce::Rectangle<int> volumeFader;
    juce::Rectangle<int> volumeText;
    juce::Rectangle<int> volumeMeter;
    juce::Rectangle<int> clipIndicator;
    juce::Rectangle<int> grip;
    juce::Rectangle<int> arm;
    juce::Rectangle<int> name;
    juce::Rectangle<int> mute;
    juce::Rectangle<int> solo;
    juce::Rectangle<int> input;
    juce::Rectangle<int> monitoring;
};

TrackRowLayout trackRowLayout (juce::Rectangle<int> row);

// Edit mode (issue #28, issue #9 resolution): the Clip toolbar, the timeline, and under a
// draggable divider the Mixer region (issue #31), which can be hidden. While the camera is Armed
// the Video Preview (issue #198) takes the Mixer region's right end, videoPreviewWidth wide at the
// region's full height, inset as the Mixer's strips are; it hides with the region.
inline constexpr int clipToolbarHeight = 36;
inline constexpr int mixerDividerHeight = 6;
inline constexpr int minimumMixerHeight = 220;
inline constexpr int defaultMixerHeight = 300;
inline constexpr int minimumTimelineHeight = 160;
inline constexpr int videoPreviewWidth = 240;

struct EditPageLayout
{
    juce::Rectangle<int> clipToolbar;
    juce::Rectangle<int> timeline;
    juce::Rectangle<int> passesButton; // over the timeline's ruler corner (issue #30)
    juce::Rectangle<int> mixerDivider; // empty when the Mixer region is hidden
    juce::Rectangle<int> mixer;        // empty when the Mixer region is hidden
    juce::Rectangle<int> videoPreview; // empty when the camera is not Armed or the Mixer region is hidden
};

// The Mixer region's height kept within [minimumMixerHeight, what leaves the timeline
// minimumTimelineHeight]; the minimum wins when the page is too short for both.
int clampMixerHeight (int requested, int pageHeight);

EditPageLayout editPageLayout (juce::Rectangle<int> page, bool mixerVisible, int mixerHeight, bool cameraArmed);

// Left: the selected Clip's details, its fade-in, fade-out and gain fields (each after its
// caption), Split, Delete. Right: zoom out, zoom in, Mixer.
struct ClipToolbarLayout
{
    juce::Rectangle<int> details;
    juce::Rectangle<int> fadeInCaption;
    juce::Rectangle<int> fadeInField;
    juce::Rectangle<int> fadeOutCaption;
    juce::Rectangle<int> fadeOutField;
    juce::Rectangle<int> gainCaption;
    juce::Rectangle<int> gainField;
    juce::Rectangle<int> split;
    juce::Rectangle<int> deleteClip;
    juce::Rectangle<int> zoomOut;
    juce::Rectangle<int> zoomIn;
    juce::Rectangle<int> mixerToggle;
};

ClipToolbarLayout clipToolbarLayout (juce::Rectangle<int> bar);

// The timeline: a header column of laneHeaderWidth on the left; on the right the ruler, the
// Camera lane, one lane per Track, each followed by its unfolded Take Lanes (issue #30), and the
// horizontal scroll bar along the bottom. Rows keep their height. Above the ruler, the Loop/Marker
// strip (issue #227) with its own corner over the header column.
//
// Vertical scrolling (issue #127): the Track rows (headers, lanes, Take Lanes) sit in the tracks
// viewport between the Camera lane and the horizontal scroll bar, moved up by the vertical scroll;
// rows outside it are cut off by the Timeline. When they are taller than the viewport, a vertical
// scroll bar of timelineScrollBarHeight runs beside them at the right edge, and the ruler, Camera
// lane, lanes and horizontal scroll bar end left of it.
inline constexpr int laneHeaderWidth = 174;
inline constexpr int rulerHeight = 24;
inline constexpr int cameraLaneHeight = 36;
inline constexpr int trackLaneHeight = 76;
inline constexpr int takeLaneHeight = 22;
inline constexpr int timelineScrollBarHeight = 12;

struct TimelineLayout
{
    juce::Rectangle<int> markerStripCorner;
    juce::Rectangle<int> markerStrip;
    juce::Rectangle<int> rulerCorner;
    juce::Rectangle<int> ruler;
    juce::Rectangle<int> cameraHeader;
    juce::Rectangle<int> cameraArm; // the Camera lane header's A, at a Track header's A (issue #198)
    juce::Rectangle<int> cameraLane;
    std::vector<juce::Rectangle<int>> trackHeaders;
    std::vector<juce::Rectangle<int>> trackLanes;
    // Per Track, one header and one lane per unfolded Take Lane (none when folded).
    std::vector<std::vector<juce::Rectangle<int>>> takeLaneHeaders;
    std::vector<std::vector<juce::Rectangle<int>>> takeLanes;
    juce::Rectangle<int> scrollBar;
    juce::Rectangle<int> tracksViewport;    // header column and lanes, where Track rows show
    juce::Rectangle<int> verticalScrollBar; // empty when the Track rows fit
    int tracksHeight = 0;                   // every Track row's height, Take Lanes included
    // Per Track, its row (its lane, then its Take Lanes) measured from the first Track's top, so
    // unchanged by the vertical scroll.
    std::vector<juce::Range<int>> trackRows;
};

// takeLanesPerTrack: one entry per Track, its number of unfolded Take Lanes (0 when folded).
// verticalScroll: how far the Track rows are moved up, used as given (see clampTimelineScroll).
TimelineLayout timelineLayout (juce::Rectangle<int> area, const std::vector<int>& takeLanesPerTrack, int verticalScroll = 0);

// The vertical scroll kept within [0, tracksHeight - viewportHeight] (0 when everything fits).
int clampTimelineScroll (int verticalScroll, int tracksHeight, int viewportHeight);

// The vertical scroll that brings the rows from spanTop to spanBottom (measured from the first
// Track's top) into a viewport of viewportHeight, moving as little as possible; their top wins
// when they are taller than the viewport.
int revealTimelineScroll (int verticalScroll, int spanTop, int spanBottom, int viewportHeight);

// Top row: the grip (issue #138), then the name. Under it, the Take Lanes toggle ("▸ 3 Takes"),
// then A, M and S, then ×.
struct TrackHeaderLayout
{
    juce::Rectangle<int> grip;
    juce::Rectangle<int> arm;
    juce::Rectangle<int> name;
    juce::Rectangle<int> takesToggle;
    juce::Rectangle<int> mute;
    juce::Rectangle<int> solo;
    juce::Rectangle<int> deleteButton;
};

TrackHeaderLayout trackHeaderLayout (juce::Rectangle<int> header);

// An Insert Chain (issue #34): "+ Insert" and one chip per slot (its name, bypass, remove).
// Horizontal (Record rows): "+ Insert" first, then chips of insertChipWidth left to right.
// Vertical (Mixer strips): chips of the area's width stacked from the top, "+ Insert" at the
// bottom. Chips that do not fit are empty (cut off).
inline constexpr int insertChipHeight = 18;
inline constexpr int insertChipWidth = 104;
inline constexpr int insertAddButtonWidth = 64;
inline constexpr int insertChipGap = 2;
// A Mixer strip's Insert Chain: three chips over "+ Insert".
inline constexpr int mixerInsertsHeight = 4 * insertChipHeight + 3 * insertChipGap;

struct InsertChainLayout
{
    std::vector<juce::Rectangle<int>> chips;
    juce::Rectangle<int> addButton;
};

InsertChainLayout insertChainLayout (juce::Rectangle<int> area, int numChips, bool vertical);

struct InsertChipLayout
{
    juce::Rectangle<int> name;
    juce::Rectangle<int> bypass;
    juce::Rectangle<int> remove;
};

InsertChipLayout insertChipLayout (juce::Rectangle<int> chip);

// Where a dragged chip lands, as an index for song::moveInsert: the number of other shown chips
// whose centre is before `point` (above it when vertical, left of it when horizontal).
std::size_t insertDropIndex (const std::vector<juce::Rectangle<int>>& chips, std::size_t dragged, juce::Point<int> point, bool vertical);

// Where a dragged Track lands (issue #138), as an index for song::moveTrack. `rows` are the
// Tracks' rows top to bottom in Song order (an Edit Track's lane with its unfolded Take Lanes, or a
// Record row), as vertical spans: the index is the number of other rows whose centre is above `y`.
std::size_t trackDropIndex (const std::vector<juce::Range<int>>& rows, std::size_t dragged, int y);

// The y of the insertion line for a drop index: midway between the other rows it falls between,
// the first one's top before them all, the last one's bottom after them all.
int trackDropLineY (const std::vector<juce::Range<int>>& rows, std::size_t dragged, std::size_t dropIndex);

// The Mixer (issue #31): one strip of mixerStripWidth per Track from the left, and the Master
// strip at the right edge. Strips that do not fit before the Master are empty (cut off).
inline constexpr int mixerStripWidth = 110;

struct MixerLayout
{
    std::vector<juce::Rectangle<int>> strips;
    juce::Rectangle<int> master;
};

MixerLayout mixerLayout (juce::Rectangle<int> mixer, int numTracks);

// Top to bottom: the name, the pan control, the Insert Chain (vertical, mixerInsertsHeight), the
// fader with its meter on its right, the dB value, then M and S side by side. The Master strip
// uses the same places and shows no pan, M or S.
struct MixerStripLayout
{
    juce::Rectangle<int> name;
    juce::Rectangle<int> pan;
    juce::Rectangle<int> inserts;
    juce::Rectangle<int> fader;
    juce::Rectangle<int> meter;
    juce::Rectangle<int> value;
    juce::Rectangle<int> mute;
    juce::Rectangle<int> solo;
};

MixerStripLayout mixerStripLayout (juce::Rectangle<int> strip);

// Edit's Passes list (issue #30): a panel under the Passes button, one row per Pass (newest
// first), each with its title, heard status and a Bring back button. The panel is kept inside
// `page`; rows past its bottom are cut off.
inline constexpr int passesPanelWidth = 560;
inline constexpr int passRowHeight = 30;
inline constexpr int passesPanelHeaderHeight = 26;

// The panel's bounds in the page.
juce::Rectangle<int> passesPanelBounds (juce::Rectangle<int> page, juce::Rectangle<int> passesButton, int numRows);

struct PassesPanelLayout
{
    juce::Rectangle<int> heading;
    std::vector<juce::Rectangle<int>> rows;
};

// Inside the panel, in its own coordinates.
PassesPanelLayout passesPanelLayout (juce::Rectangle<int> panel, int numRows);

struct PassRowLayout
{
    juce::Rectangle<int> title;
    juce::Rectangle<int> status;
    juce::Rectangle<int> bringBack;
};

PassRowLayout passRowLayout (juce::Rectangle<int> row);

// Transcribe mode (issue #225): the whole-Song overview strip on top, under it the Loop/Marker
// strip (issue #227) over the ruler over the one tall waveform; at the left, full height and
// transcribeMarkersWidth wide, the Loop cards (issue #228) in its top half over the Markers list
// (issue #227), and at the right, full height, the
// Transcribe column of transcribeColumnWidth while it shows (empty while hidden: the rest takes its
// width).
inline constexpr int transcribeOverviewHeight = 56;
inline constexpr int transcribeRulerHeight = 22;
inline constexpr int transcribeColumnWidth = 200;
inline constexpr int transcribeMarkersWidth = 180;

struct TranscribePageLayout
{
    juce::Rectangle<int> loops;
    juce::Rectangle<int> markers;
    juce::Rectangle<int> overview;
    juce::Rectangle<int> markerStrip;
    juce::Rectangle<int> ruler;
    juce::Rectangle<int> waveform;
    juce::Rectangle<int> column;
};

TranscribePageLayout transcribePageLayout (juce::Rectangle<int> page, bool columnVisible);

// Inside the Transcribe column, in its own coordinates: the "LISTENING" heading, then Practice
// Speed and Transpose, each a caption over its field, unit and spinner (Transpose's "st" followed
// by its cents field and "ct", issue #232), then the "TRANSCRIBE MIX"
// caption over the Track list (issue #226): one row of transcribeTrackRowHeight per Track, in Song
// order, from the top of `tracks`.
inline constexpr int transcribeTrackRowHeight = 26;

struct TranscribeColumnLayout
{
    juce::Rectangle<int> heading;
    juce::Rectangle<int> practiceSpeedCaption;
    juce::Rectangle<int> practiceSpeedField;
    juce::Rectangle<int> practiceSpeedUnit;
    juce::Rectangle<int> practiceSpeedSpinner;
    juce::Rectangle<int> transposeCaption;
    juce::Rectangle<int> transposeField;
    juce::Rectangle<int> transposeUnit;
    juce::Rectangle<int> transposeCentsField;
    juce::Rectangle<int> transposeCentsUnit;
    juce::Rectangle<int> transposeSpinner;
    juce::Rectangle<int> mixCaption;
    juce::Rectangle<int> tracks;
};

TranscribeColumnLayout transcribeColumnLayout (juce::Rectangle<int> column);

// One row of the Transcribe column's Track list, in its own coordinates: the Track's colour swatch
// and name, then ◎ (Focus), M and S at the right.
struct TranscribeTrackRowLayout
{
    juce::Rectangle<int> swatch;
    juce::Rectangle<int> name;
    juce::Rectangle<int> focus;
    juce::Rectangle<int> mute;
    juce::Rectangle<int> solo;
};

TranscribeTrackRowLayout transcribeTrackRowLayout (juce::Rectangle<int> row);
} // namespace overdub::ui
