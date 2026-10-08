#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <vector>

// The main window's geometry (ADR 0005 UI amendment): pure functions from a bounds rectangle to
// child rectangles, so Components only call setBounds with what these return. Sizes in points.
namespace overdub::ui
{
inline constexpr int minimumWindowWidth = 960;
inline constexpr int minimumWindowHeight = 600;
inline constexpr int topBarHeight = 52;
inline constexpr int noticeLineHeight = 28;
inline constexpr int trackColumnWidth = 460;
inline constexpr int trackRowHeight = 40;

struct MainLayout
{
    juce::Rectangle<int> topBar;
    juce::Rectangle<int> page;       // the current mode's page
    juce::Rectangle<int> noticeLine; // over the bottom of the page, shown only with a Notice
};

MainLayout mainLayout (juce::Rectangle<int> window);

// Left: Record · Edit · Export. Centre: ⏮ ▶/■ ● and the clock. Right: Undo, Redo.
struct TopBarLayout
{
    std::array<juce::Rectangle<int>, 3> modeButtons;
    juce::Rectangle<int> rewind;
    juce::Rectangle<int> playStop;
    juce::Rectangle<int> record;
    juce::Rectangle<int> clock;
    juce::Rectangle<int> undo;
    juce::Rectangle<int> redo;
};

TopBarLayout topBarLayout (juce::Rectangle<int> bar);

// Bare Record mode: the stage on the left (the camera stage arrives in slice 14), the Track rows
// in a right column of trackColumnWidth, the Import button under the last row.
struct RecordPageLayout
{
    juce::Rectangle<int> stage;
    std::vector<juce::Rectangle<int>> rows;
    juce::Rectangle<int> importButton;
};

RecordPageLayout recordPageLayout (juce::Rectangle<int> page, int numTracks);

struct TrackRowLayout
{
    juce::Rectangle<int> arm;
    juce::Rectangle<int> name;
    juce::Rectangle<int> input;
    juce::Rectangle<int> monitoring;
};

TrackRowLayout trackRowLayout (juce::Rectangle<int> row);

// Edit mode (issue #28, issue #9 resolution): the Clip toolbar, the timeline, and under a
// draggable divider the Mixer region (empty until slice 10), which can be hidden.
inline constexpr int clipToolbarHeight = 36;
inline constexpr int mixerDividerHeight = 6;
inline constexpr int minimumMixerHeight = 120;
inline constexpr int defaultMixerHeight = 220;
inline constexpr int minimumTimelineHeight = 160;

struct EditPageLayout
{
    juce::Rectangle<int> clipToolbar;
    juce::Rectangle<int> timeline;
    juce::Rectangle<int> passesButton; // over the timeline's ruler corner (issue #30)
    juce::Rectangle<int> mixerDivider; // empty when the Mixer region is hidden
    juce::Rectangle<int> mixer;        // empty when the Mixer region is hidden
};

// The Mixer region's height kept within [minimumMixerHeight, what leaves the timeline
// minimumTimelineHeight]; the minimum wins when the page is too short for both.
int clampMixerHeight (int requested, int pageHeight);

EditPageLayout editPageLayout (juce::Rectangle<int> page, bool mixerVisible, int mixerHeight);

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
// horizontal scroll bar along the bottom. Lanes past the bottom are cut off (rows keep their
// height).
inline constexpr int laneHeaderWidth = 150;
inline constexpr int rulerHeight = 24;
inline constexpr int cameraLaneHeight = 36;
inline constexpr int trackLaneHeight = 76;
inline constexpr int takeLaneHeight = 22;
inline constexpr int timelineScrollBarHeight = 12;

struct TimelineLayout
{
    juce::Rectangle<int> rulerCorner;
    juce::Rectangle<int> ruler;
    juce::Rectangle<int> cameraHeader;
    juce::Rectangle<int> cameraLane;
    std::vector<juce::Rectangle<int>> trackHeaders;
    std::vector<juce::Rectangle<int>> trackLanes;
    // Per Track, one header and one lane per unfolded Take Lane (none when folded).
    std::vector<std::vector<juce::Rectangle<int>>> takeLaneHeaders;
    std::vector<std::vector<juce::Rectangle<int>>> takeLanes;
    juce::Rectangle<int> scrollBar;
};

// takeLanesPerTrack: one entry per Track, its number of unfolded Take Lanes (0 when folded).
TimelineLayout timelineLayout (juce::Rectangle<int> area, const std::vector<int>& takeLanesPerTrack);

// Top row: arm, then the name. Under it, the Take Lanes toggle ("▸ 3 Takes").
struct TrackHeaderLayout
{
    juce::Rectangle<int> arm;
    juce::Rectangle<int> name;
    juce::Rectangle<int> takesToggle;
};

TrackHeaderLayout trackHeaderLayout (juce::Rectangle<int> header);

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
} // namespace overdub::ui
