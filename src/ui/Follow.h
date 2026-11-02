#pragma once

#include "song/Song.h"
#include "ui/Mode.h"
#include "ui/TimelineGeometry.h"

// Follow (issue #266, GLOSSARY): the view moving with the playhead while the Song plays. Edit's
// Timeline pages and Transcribe's waveform scrolls; neither changes the zoom. The two functions are
// pure: where Follow would put a page's view now. ui::Follow is a page's pause state over them, and
// the one part that logs. ui::Timeline and ui::TranscribePage only call these.
namespace overdub::ui
{
// Edit pages the playhead to 1/editFollowLeadParts of the lanes' width in; Transcribe holds it at
// 1/transcribeFollowSpotParts of the waveform's width.
inline constexpr int editFollowLeadParts = 10;
inline constexpr int transcribeFollowSpotParts = 5;

// Where Edit's lanes (`width` pixels from firstSample) start: unchanged while the playhead is inside
// them, else paged so the playhead sits a tenth of their width in from the left, never before 0.
SampleCount editFollowFirstSample (const TimelineView& view, int width, SampleCount playhead);

// Where Transcribe's waveform (`width` pixels from firstSample) starts, within [0, songLength - its
// span] (ui::clampedTranscribeView). `loop` is the active Loop while looping, else null. With the
// playhead inside a Loop that fits in the span, the view moves the least that shows the whole Loop
// (not at all while it does). Otherwise the playhead walks right until it is a fifth of the width
// in, then the waveform scrolls to hold it there; a playhead before the view (a Loop's wrap, a
// seek) brings the view back to put it there.
SampleCount transcribeFollowFirstSample (const TimelineView& view, int width, SampleCount songLength, SampleCount playhead, const song::Loop* loop);

enum class FollowResume
{
    play,
    seek,
    inView
};

// "play", "seek" or "in_view": the reason in ui.follow_resumed.
const char* followResumeName (FollowResume reason);

// A page's Follow. A hand-made view change pauses it when Follow would move the view from where it
// was put; the next play, a seek, or Follow coming to leave the view where it is (the playhead back
// in Edit's lanes, or at or before its fifth of Transcribe's waveform) resumes it. Each change is
// logged once: ui.follow_paused {mode}, ui.follow_resumed {mode, reason}.
class Follow
{
public:
    explicit Follow (Mode pageMode) : mode (pageMode) {}

    bool isPaused() const { return paused; }

    // The view now starts at `shown` by hand; Follow would put it at `followed`.
    void viewSetByHand (SampleCount shown, SampleCount followed);
    void resume (FollowResume reason);

    // Where the view starts at a refresh: `followed` while playing and not paused, else `shown`.
    // A paused Follow resumes (in_view) first when `followed` is `shown`.
    SampleCount firstSample (SampleCount shown, SampleCount followed, bool playing);

private:
    Mode mode;
    bool paused = false;
};
} // namespace overdub::ui
