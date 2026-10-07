// PROTOTYPE — issue #6 spike. Graph nodes owned by SpikeEngine's AudioProcessorGraph.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

// Timeline position shared by every node. The engine sets blockStart before the graph
// runs a block and advances it afterwards, so every node sees the same value per block.
struct Transport
{
    std::atomic<bool> playing { false };
    std::atomic<juce::int64> blockStart { 0 };
};

// Boilerplate defaults so the real nodes below only show what matters.
class SpikeProcessorBase : public juce::AudioProcessor
{
public:
    using juce::AudioProcessor::AudioProcessor;
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

// Plays an in-memory stereo buffer at the transport position. 0 in, 2 out.
// The message thread swaps the buffer only while stopped; it keeps old buffers alive.
class BackingPlayer final : public SpikeProcessorBase
{
public:
    explicit BackingPlayer (Transport&);
    const juce::String getName() const override { return "BackingPlayer"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) [[clang::nonblocking]] override;
    void setBuffer (const juce::AudioBuffer<float>* buffer) { current.store (buffer); }

private:
    Transport& transport;
    std::atomic<const juce::AudioBuffer<float>*> current { nullptr };
};

// Monitoring on/off for one track. 2 in, 2 out.
class MonitorGain final : public SpikeProcessorBase
{
public:
    MonitorGain();
    const juce::String getName() const override { return "MonitorGain"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) [[clang::nonblocking]] override;
    std::atomic<float> gain { 0.0f };
};

// Taps every device input dry (before any plugin) and writes armed tracks to mono WAVs.
// N in, 0 out. Connected straight to the graph input node, so PDC never delays it.
class DryRecorder final : public SpikeProcessorBase
{
public:
    static constexpr int numTracks = 2;

    DryRecorder (int numInputs, juce::TimeSliceThread& writerThread);
    const juce::String getName() const override { return "DryRecorder"; }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) [[clang::nonblocking]] override;

    // Message thread only.
    bool startTake (int track, int inputChannel, const juce::File& file, double sampleRate);
    void stopAllTakes();

    struct TakeStats
    {
        juce::File file;
        juce::int64 firstBlockStart = -1;   // timeline sample of the take's first dry sample
        juce::int64 samplesWritten = 0;
        juce::int64 samplesDropped = 0;
    };
    TakeStats getStats (int track) const;
    juce::uint64 getProcessedBlocks() const { return processedBlocks.load(); }

private:
    struct Slot
    {
        std::atomic<juce::AudioFormatWriter::ThreadedWriter*> writer { nullptr };
        std::atomic<int> inputChannel { 0 };
        std::atomic<juce::int64> firstBlockStart { -1 };
        std::atomic<juce::int64> samplesWritten { 0 };
        std::atomic<juce::int64> samplesDropped { 0 };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> owned;
        juce::File file;
    };

    juce::TimeSliceThread& writerThread;
    Slot slots[numTracks];
    std::atomic<juce::uint64> processedBlocks { 0 };
    Transport* transport = nullptr;

public:
    void setTransport (Transport& t) { transport = &t; }
};
