#pragma once

#include "engine/PlainProcessor.h"
#include "engine/RenderPlan.h"

#include <memory>
#include <vector>

// Test-only processors (ADR 0005): gain, a fixed N-sample delay that reports N samples of
// latency, and silence. Stereo in, stereo out, like every node on a Track's path.
namespace overdub::engine::testing
{
class GainProcessor final : public PlainProcessor
{
public:
    explicit GainProcessor (float linearGain) : PlainProcessor (stereoInOut()), gain (linearGain) {}

    const juce::String getName() const override { return "Test Gain"; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain (gain); }

private:
    float gain;
};

class DelayProcessor final : public PlainProcessor
{
public:
    explicit DelayProcessor (int delaySamples) : PlainProcessor (stereoInOut()), delay (delaySamples)
    {
        // Off the audio thread: setLatencySamples notifies listeners.
        setLatencySamples (delay);
    }

    const juce::String getName() const override { return "Test Delay"; }

    void prepareToPlay (double, int) override
    {
        history.assign (2, std::vector<float> ((std::size_t) delay, 0.0f));
        writeIndex = 0;
    }

    void reset() override
    {
        for (auto& channel : history)
            std::fill (channel.begin(), channel.end(), 0.0f);
        writeIndex = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (delay == 0)
            return;

        const auto startIndex = writeIndex;
        for (int channel = 0; channel < std::min (buffer.getNumChannels(), 2); ++channel)
        {
            auto& line = history[(std::size_t) channel];
            auto index = startIndex;
            auto* samples = buffer.getWritePointer (channel);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                std::swap (samples[i], line[index]);
                index = (index + 1) % line.size();
            }
            writeIndex = index;
        }
    }

private:
    int delay;
    std::vector<std::vector<float>> history;
    std::size_t writeIndex = 0;
};

class SilenceProcessor final : public PlainProcessor
{
public:
    SilenceProcessor() : PlainProcessor (stereoInOut()) {}

    const juce::String getName() const override { return "Test Silence"; }
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
};

// An effect delay: adds its input delayed by `delaySamples` to itself, reports no latency and a
// tail of `delaySamples` (the echo it still owes once its input falls silent).
class EchoProcessor final : public PlainProcessor
{
public:
    EchoProcessor (int delaySamples, int sampleRate) : PlainProcessor (stereoInOut()), delay (delaySamples), rate (sampleRate) {}

    const juce::String getName() const override { return "Test Echo"; }
    double getTailLengthSeconds() const override { return (double) delay / rate; }

    void prepareToPlay (double, int) override
    {
        history.assign (2, std::vector<float> ((std::size_t) delay, 0.0f));
        writeIndex = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const auto startIndex = writeIndex;
        for (int channel = 0; channel < std::min (buffer.getNumChannels(), 2); ++channel)
        {
            auto& line = history[(std::size_t) channel];
            auto index = startIndex;
            auto* samples = buffer.getWritePointer (channel);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto echoed = line[index];
                line[index] = samples[i];
                samples[i] += echoed;
                index = (index + 1) % line.size();
            }
            writeIndex = index;
        }
    }

private:
    int delay;
    int rate;
    std::vector<std::vector<float>> history;
    std::size_t writeIndex = 0;
};

// An InsertFactory that puts `makeProcessor()` on the Track with this id and nothing elsewhere.
template <typename MakeProcessor>
InsertFactory insertOnTrack (std::string trackId, MakeProcessor makeProcessor)
{
    return [trackId, makeProcessor] (const song::Id& chainTrackId, const std::vector<song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (chainTrackId == trackId)
            inserts.push_back (makeProcessor());
        return inserts;
    };
}

// An InsertFactory that puts `makeProcessor()` on the Master and nothing on any Track.
template <typename MakeProcessor>
InsertFactory insertOnMaster (MakeProcessor makeProcessor)
{
    return [makeProcessor] (const song::Id& chainTrackId, const std::vector<song::InsertSlot>&) {
        std::vector<std::unique_ptr<juce::AudioProcessor>> inserts;
        if (chainTrackId.empty())
            inserts.push_back (makeProcessor());
        return inserts;
    };
}
} // namespace overdub::engine::testing
