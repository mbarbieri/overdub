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
} // namespace overdub::ui
