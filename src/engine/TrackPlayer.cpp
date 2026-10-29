#include "engine/TrackPlayer.h"

#include <algorithm>

namespace overdub::engine
{
TrackPlayer::TrackPlayer (juce::String id,
                          std::vector<ClipSegment> clipSegments,
                          std::vector<SourceAudio> sourceAudio,
                          const LiveInput& input,
                          std::vector<int> channelsToMonitor)
    : PlainProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      trackId (std::move (id)),
      segments (std::move (clipSegments)),
      sources (std::move (sourceAudio)),
      liveInput (input),
      monitorChannels (std::move (channelsToMonitor))
{
}

void TrackPlayer::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};

    if (! position.hasValue() || ! position->getIsPlaying() || ! position->getTimeInSamples().hasValue())
        buffer.clear();
    else
        renderSegments (segments, *position->getTimeInSamples(), buffer);

    addMonitoredInput (buffer);
}

void TrackPlayer::addMonitoredInput (juce::AudioBuffer<float>& buffer) const noexcept
{
    if (! liveInput.monitoring || monitorChannels.empty() || liveInput.channels == nullptr)
        return;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const auto inputChannel = monitorChannels[(std::size_t) std::min (channel, (int) monitorChannels.size() - 1)];
        if (inputChannel < 0 || inputChannel >= liveInput.numChannels || liveInput.channels[inputChannel] == nullptr)
            continue;
        buffer.addFrom (channel, 0, liveInput.channels[inputChannel] + liveInput.firstFrame, buffer.getNumSamples());
    }
}
} // namespace overdub::engine
