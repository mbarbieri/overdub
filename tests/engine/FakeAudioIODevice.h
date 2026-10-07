#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

// A juce::AudioIODevice that touches no hardware: it reports the shape it was given and never
// runs a callback by itself. Tests hand it to DeviceBinding::audioDeviceAboutToStart and call the
// binding's callback directly.
namespace overdub::engine::testing
{
struct FakeDeviceShape
{
    juce::String name = "Fake Device";
    juce::String typeName = "Fake";
    int numInputChannels = 2;
    int numOutputChannels = 2;
    double sampleRate = 48000.0;
    int bufferSize = 256;
    int inputLatencySamples = 0;
    int outputLatencySamples = 0;
    int xruns = 0;
};

class FakeAudioIODevice final : public juce::AudioIODevice
{
public:
    explicit FakeAudioIODevice (FakeDeviceShape deviceShape)
        : juce::AudioIODevice (deviceShape.name, deviceShape.typeName), shape (deviceShape)
    {
    }

    juce::StringArray getOutputChannelNames() override { return channelNames (shape.numOutputChannels); }
    juce::StringArray getInputChannelNames() override { return channelNames (shape.numInputChannels); }
    juce::Array<double> getAvailableSampleRates() override { return { shape.sampleRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { shape.bufferSize }; }
    int getDefaultBufferSize() override { return shape.bufferSize; }

    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }

    int getCurrentBufferSizeSamples() override { return shape.bufferSize; }
    double getCurrentSampleRate() override { return shape.sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return firstChannels (shape.numOutputChannels); }
    juce::BigInteger getActiveInputChannels() const override { return firstChannels (shape.numInputChannels); }
    int getOutputLatencyInSamples() override { return shape.outputLatencySamples; }
    int getInputLatencyInSamples() override { return shape.inputLatencySamples; }
    int getXRunCount() const noexcept override { return shape.xruns; }

    FakeDeviceShape shape;

private:
    static juce::StringArray channelNames (int count)
    {
        juce::StringArray names;
        for (int channel = 0; channel < count; ++channel)
            names.add ("Channel " + juce::String (channel + 1));
        return names;
    }

    static juce::BigInteger firstChannels (int count)
    {
        juce::BigInteger channels;
        channels.setRange (0, count, true);
        return channels;
    }
};
} // namespace overdub::engine::testing
