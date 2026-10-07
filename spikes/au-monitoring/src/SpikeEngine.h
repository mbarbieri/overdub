// PROTOTYPE — issue #6 spike. One AudioProcessorGraph driven straight from the device callback.
//
// Graph:  input ─┬─ ch c ─> [plugin t] ─> MonitorGain t ─┐
//                └─ all ─> DryRecorder (no PDC delay)     ├─> output
//         BackingPlayer ──────────────────────────────────┘
#pragma once
#include "SpikeProcessors.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

class SpikeEngine final : public juce::AudioIODeviceCallback
{
public:
    static constexpr int numTracks = DryRecorder::numTracks;

    SpikeEngine();
    ~SpikeEngine() override;

    juce::AudioDeviceManager deviceManager;

    // Device
    juce::String openDefaultDevice();
    juce::String setBufferSize (int samples);

    // Tracks (message thread)
    juce::StringArray listEffectNames();
    void loadPlugin (int track, const juce::String& name, bool outOfProcess, std::function<void (juce::String error)> done);
    void unloadPlugin (int track);
    juce::AudioPluginInstance* getPlugin (int track);
    void setMonitoring (int track, bool on);
    bool getMonitoring (int track) const { return tracks[track].monitoring; }
    void setInputChannel (int track, int channel);
    int getInputChannel (int track) const { return tracks[track].inputChannel; }
    int getNumGraphInputs() const { return numGraphInputs; }

    // Backing track (message thread, while stopped)
    juce::String loadBackingFile (const juce::File& file);
    void setBackingBuffer (juce::AudioBuffer<float> buffer);

    // Transport (message thread)
    void start (const bool armed[numTracks]);
    void stop();
    bool isPlaying() const { return transport.playing.load(); }
    juce::int64 getPosition() const { return transport.blockStart.load(); }

    struct Latency
    {
        double sampleRate = 0;
        int bufferSize = 0;
        int input = 0;
        int output = 0;
        int graph = 0;
        int total() const { return input + output + graph; }
    };
    Latency getLatency();
    Latency getLatencyAtLastStart() const { return latencyAtLastStart; }
    DryRecorder::TakeStats getTakeStats (int track) const { return recorder->getStats (track); }

    struct Health
    {
        double cpu = 0;
        int xruns = 0;
        double maxCallbackMs = 0;
        double budgetMs = 0;
    };
    Health getHealth();
    void resetMaxCallback() { maxCallbackTicks.store (0); }

    juce::StringPairArray describeTrack (int track);
    static juce::String countHostingProcesses();

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) [[clang::nonblocking]] override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    struct Track
    {
        int inputChannel = 0;
        bool monitoring = false;
        bool outOfProcessRequested = false;
        juce::String pluginName;
        double loadMs = 0;
        juce::AudioProcessorGraph::NodeID pluginNode {};
        juce::AudioProcessorGraph::NodeID gainNode {};
    };

    void buildGraph();
    void rewireTrack (int track);
    juce::String findIdentifier (const juce::String& name);

    juce::AudioPluginFormatManager formats;
    juce::AudioProcessorGraph graph;
    juce::TimeSliceThread writerThread { "take writer" };
    Transport transport;

    juce::AudioProcessorGraph::NodeID inputNode, outputNode, backingNode, recorderNode;
    BackingPlayer* backing = nullptr;
    DryRecorder* recorder = nullptr;
    Track tracks[numTracks];
    int numGraphInputs = 0;
    int numGraphOutputs = 0;

    std::vector<std::unique_ptr<juce::AudioBuffer<float>>> backingBuffers;   // kept alive; audio thread may hold one
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    std::atomic<juce::int64> maxCallbackTicks { 0 };
    Latency latencyAtLastStart;
};
