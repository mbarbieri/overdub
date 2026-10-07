#include "engine/TrackPlayer.h"

namespace overdub::engine
{
TrackPlayer::TrackPlayer (juce::String id, std::vector<ClipSegment> clipSegments, std::vector<SourceAudio> sourceAudio)
    : PlainProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      trackId (std::move (id)),
      segments (std::move (clipSegments)),
      sources (std::move (sourceAudio))
{
}

void TrackPlayer::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};

    if (! position.hasValue() || ! position->getIsPlaying() || ! position->getTimeInSamples().hasValue())
    {
        buffer.clear();
        return;
    }

    renderSegments (segments, *position->getTimeInSamples(), buffer);
}
} // namespace overdub::engine
