#include "export/MixRender.h"

#include <algorithm>

namespace overdub::exporting
{
song::SampleCount lastClipEnd (const song::Song& song)
{
    song::SampleCount end = 0;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
            end = std::max (end, clip.timelineStart + clip.length);
    return end;
}

MixRender::MixRender (const song::Song& song, const juce::File& packageRoot, int maxBlockSize, const engine::InsertFactory& makeInserts)
    : plan (std::make_unique<engine::RenderPlan> (song, packageRoot, engine::EngineConfig { .numOutputChannels = numChannels, .maxBlockSize = maxBlockSize }, makeInserts)),
      songEndSample (lastClipEnd (song)),
      scratch (numChannels, maxBlockSize)
{
}

int MixRender::renderNext (float* const* outputs, int numSamples)
{
    skipLatency();
    const auto count = (int) std::clamp<song::SampleCount> (totalSamples() - produced, 0, std::min (numSamples, maxBlockSize()));
    if (count == 0)
        return 0;

    plan->render (outputs, engine::LiveInput {}, 0, count, graphPosition, true);
    graphPosition += count;
    produced += count;
    return count;
}

void MixRender::skipLatency()
{
    while (graphPosition < latencySamples())
    {
        const auto count = (int) std::min<song::SampleCount> (latencySamples() - graphPosition, maxBlockSize());
        plan->render (scratch.getArrayOfWritePointers(), engine::LiveInput {}, 0, count, graphPosition, true);
        graphPosition += count;
    }
}
} // namespace overdub::exporting
