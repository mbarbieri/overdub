#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

// A juce::AudioProcessor with no editor, programs, MIDI or state: the base of the engine's own
// graph nodes (Track players, Master) and of the test-only processors (ADR 0005).
namespace overdub::engine
{
class PlainProcessor : public juce::AudioProcessor
{
public:
    explicit PlainProcessor (const BusesProperties& buses) : juce::AudioProcessor (buses) {}

    // Stereo in and out: the shape of every node on a Track's path to Master.
    static BusesProperties stereoInOut()
    {
        return BusesProperties()
            .withInput ("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
    }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};
} // namespace overdub::engine
