#pragma once

#include "song/Song.h"
#include "ui/RecordView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

// Record mode's whole-Song overview (issue #32, issue #9 resolution). Thin: it draws with the
// geometry of ui/RecordView.h and reports a click as the Song sample under it.
namespace overdub::ui
{
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
