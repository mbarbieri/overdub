#pragma once

#include "song/Song.h"
#include "ui/RecordView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// Record mode's Backing Track look-ahead strip (issue #32, issue #9 resolution). Thin: it draws
// what ui/RecordView.h computes.
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

} // namespace overdub::ui
