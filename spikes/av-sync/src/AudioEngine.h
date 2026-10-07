// PROTOTYPE — issue #7 spike.
#pragma once
#include "ClockMap.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

// Duplex device callback with a sample-counting transport (song sample 0 = start of
// recording or playback). It records input channel 0 dry, plays a mono song mix, and
// publishes one ClockAnchor per callback. Public methods are message-thread only.
class AudioEngine final : private juce::AudioIODeviceCallback,
                          private juce::Timer
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::String openDefaultDevice();

    double getSampleRate() const;
    int getBufferSize() const;
    int getInputLatencySamples() const;
    int getOutputLatencySamples() const;

    // Input before recordFromSample is discarded, so the Take never starts before song 0.
    juce::String startRecording (const juce::File& wavFile, int64_t recordFromSample);
    struct RecordingResult
    {
        int64_t firstRecordedSample = -1;
        int64_t samplesWritten = 0;
        std::vector<ClockAnchor> anchors;
    };
    RecordingResult stopRecording();

    void startPlayback (juce::AudioBuffer<float> songMix);
    std::vector<ClockAnchor> stopPlayback();
    bool isPlaying() const { return mode.load() == Mode::playing; }
    bool playbackReachedEnd() const;

    // Song sample heard at hostNs (output IO time + output latency), from the latest anchor at the nominal rate.
    bool songSampleHeardAt (uint64_t hostNs, double& songSample) const;

private:
    enum class Mode { idle, recording, playing };

    void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                           float* const* output, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void timerCallback() override;

    void publishLatestAnchor (ClockAnchor anchor);
    bool readLatestAnchor (ClockAnchor& anchor) const;
    void drainAnchors();
    void waitForCallbacksToPass();

    juce::AudioDeviceManager deviceManager;
    std::atomic<Mode> mode { Mode::idle };
    std::atomic<int64_t> transportSample { 0 };

    // Recording state. The writer pointer is swapped out before the writer is destroyed.
    juce::TimeSliceThread writerThread { "take writer" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    std::atomic<int64_t> recordFromSample { 0 };
    std::atomic<int64_t> firstRecordedSample { -1 };
    std::atomic<int64_t> samplesWritten { 0 };
    std::atomic<int> writeFailures { 0 };

    // Playback state. The buffer is only touched by the callback while mode == playing.
    juce::AudioBuffer<float> playbackMix;

    // Anchors: callback → FIFO → message thread vector.
    juce::AbstractFifo anchorFifo { 1 << 16 };
    std::vector<ClockAnchor> anchorRing = std::vector<ClockAnchor> (1 << 16);
    std::vector<ClockAnchor> collectedAnchors;
    std::atomic<int> droppedAnchors { 0 };

    // Seqlock for the latest anchor (read by the video display link).
    std::atomic<uint32_t> latestSequence { 0 };
    std::atomic<int64_t> latestSong { -1 };
    std::atomic<uint64_t> latestHostNs { 0 };
};
