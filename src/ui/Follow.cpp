#include "ui/Follow.h"

#include "core/Log.h"
#include "ui/TranscribeGeometry.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// The samples `pixels` columns of the view span (at least one column).
SampleCount samplesAcross (const TimelineView& view, int pixels)
{
    return view.samplesPerPixel * (SampleCount) std::max (1, pixels);
}
} // namespace

SampleCount editFollowFirstSample (const TimelineView& view, int width, SampleCount playhead)
{
    if (playhead >= view.firstSample && playhead < view.firstSample + samplesAcross (view, width))
        return view.firstSample;
    return std::max<SampleCount> (0, playhead - samplesAcross (view, width / editFollowLeadParts));
}

SampleCount transcribeFollowFirstSample (const TimelineView& view, int width, SampleCount songLength, SampleCount playhead, const song::Loop* loop)
{
    const auto span = samplesAcross (view, width);
    auto firstSample = view.firstSample;
    const auto inLoop = loop != nullptr && playhead >= loop->start && playhead < loop->end;
    if (inLoop && loop->length() <= span)
    {
        if (loop->start < firstSample)
            firstSample = loop->start;
        else if (loop->end > firstSample + span)
            firstSample = loop->end - span;
    }
    else
    {
        const auto spot = samplesAcross (view, width / transcribeFollowSpotParts);
        firstSample = playhead < firstSample ? playhead - spot : std::max (firstSample, playhead - spot);
    }
    auto followed = view;
    followed.firstSample = firstSample;
    return clampedTranscribeView (followed, view.samplesPerPixel, view.samplesPerPixel, songLength, width).firstSample;
}

const char* followResumeName (FollowResume reason)
{
    switch (reason)
    {
        case FollowResume::play: return "play";
        case FollowResume::seek: return "seek";
        case FollowResume::inView: return "in_view";
    }
    return "unknown";
}

void Follow::viewSetByHand (SampleCount shown, SampleCount followed)
{
    if (paused || followed == shown)
        return;
    paused = true;
    core::logEvent (core::Level::info, "ui.follow_paused", { { "mode", modeName (mode) } });
}

void Follow::resume (FollowResume reason)
{
    if (! paused)
        return;
    paused = false;
    core::logEvent (core::Level::info, "ui.follow_resumed", { { "mode", modeName (mode) }, { "reason", followResumeName (reason) } });
}

SampleCount Follow::firstSample (SampleCount shown, SampleCount followed, bool playing)
{
    if (followed == shown)
        resume (FollowResume::inView);
    return playing && ! paused ? followed : shown;
}
} // namespace overdub::ui
