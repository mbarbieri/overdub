#pragma once

#include "song/Song.h"
#include "ui/RecordView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

// Record mode's Backing Track look-ahead strip and whole-Song overview (issue #32, issue #9
// resolution). Thin: they draw what ui/RecordView.h computes, and the overview reports a click as
// the Song sample under it.
namespace overdub::ui
{
// "BACKING TRACK · WHAT'S COMING" over a 30 s window of the audible Backing Track envelope, the
// past 20% shaded, the playhead fixed at 20% (red while a Pass records), every 5 s labelled, the
// running Pass tinted red up to the playhead, the record point marked during pre-roll, and the
// countdowns in the corners.
class LookAheadStrip final : public juce::Component
{
public:
    LookAheadStrip();

    void show (const LookAheadView& view, int sampleRate);
    void paint (juce::Graphics& g) override;

    // Where the envelope is drawn, in local coordinates: one column per pixel of its width.
    juce::Rectangle<int> waveformBounds() const;

    const LookAheadView& currentView() const { return view; }

private:
    LookAheadView view;
    int rate = 48000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LookAheadStrip)
};

// One thin row per Track with its Clips (Takes and Backing Tracks in their colours) across the
// whole Song, and the playhead. A click moves the playhead there.
class SongOverview final : public juce::Component
{
public:
    SongOverview();

    void show (const song::Song& song, SampleCount playhead);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;

    std::function<void (SampleCount sample)> onSeek;

    SampleCount length() const { return songLength; }

private:
    struct ClipBar
    {
        SampleCount start = 0;
        SampleCount end = 0;
        bool backing = false;

        bool operator== (const ClipBar&) const = default;
    };

    std::vector<std::vector<ClipBar>> rows; // per Track, in layer order
    SampleCount songLength = 1;
    SampleCount playhead = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongOverview)
};
} // namespace overdub::ui
