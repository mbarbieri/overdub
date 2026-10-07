// PROTOTYPE — issue #7 spike.
#pragma once
#include "AudioEngine.h"
#include "CameraRecorder.h"
#include "Session.h"
#include "VideoView.h"
#include <juce_gui_basics/juce_gui_basics.h>

struct SpikeOptions
{
    int rtCalibrationSamples = 821;   // ADR 0002: built-in devices over-report the round trip by ~821 samples at 44.1 kHz
    double cameraCalibrationMs = 0;   // per-camera latency, measured by this spike
    int videoLeadInMs = 2000;         // camera starts this long after the Take, so the offset is non-zero
    int videoLeadOutMs = 2000;        // and stops this long before it, so the full-song export has black at both ends
    int prerollMs = 1000;
};

// Record → Stop → Play → Export → Analyse on one session at a time.
class SpikeComponent final : public juce::Component
{
public:
    SpikeComponent (AudioEngine* engine, CameraRecorder* camera, SpikeOptions options);
    ~SpikeComponent() override;

    void appendLog (const juce::String& line);
    bool loadExistingSession (const juce::File& dir);
    void exportThenAnalyse (std::function<void()> onDone);
    void analyse (std::function<void()> onDone);
    void startPlayback (std::function<void()> onStopped = {});

    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    void startRecording();
    void stopRecording();
    void finishRecording (CameraTakeResult cameraResult);
    void stopPlayback();
    void onVBlank();
    void updateButtons();
    void setStatus (const juce::String& text);

    AudioEngine* engine;     // null in headless runs
    CameraRecorder* camera;  // null in headless runs
    SpikeOptions options;

    SessionInfo session;
    bool hasSession = false;
    enum class State { idle, recording, stopping, playing, busy } state = State::idle;
    int recordingId = 0;
    bool cameraWriting = false;

    std::vector<std::string> vblankRows;
    std::function<void()> onPlaybackStopped;
    juce::VBlankAttachment vblank { this, [this] { onVBlank(); } };

    VideoView videoView;
    juce::TextButton recordButton { "Record" }, stopButton { "Stop" }, playButton { "Play" },
                     exportButton { "Export MP4s + analyse" }, analyseButton { "Analyse" };
    juce::Label statusLabel;
    juce::TextEditor logView;
};
