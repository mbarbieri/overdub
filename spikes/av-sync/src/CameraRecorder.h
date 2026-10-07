// PROTOTYPE — issue #7 spike.
#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <memory>

struct CameraTakeResult
{
    bool ok = false;
    juce::String error;
    int64_t firstPtsNs = 0;      // capture-clock PTS of the first written frame
    int64_t firstHostNs = 0;     // same instant via CMSyncConvertTime → host clock
    int frames = 0;
    int writerDrops = 0;         // writer input not ready
    int captureDrops = 0;        // didDropSampleBuffer
    double durationSeconds = 0;
    double relativeRateStart = 1.0; // CMSyncGetRelativeRate(captureClock, hostClock)
    double relativeRateEnd = 1.0;
    double maxHostVsPtsDivergenceUs = 0; // |Δhost − Δpts| over the take: capture clock vs host drift
    double meanFrameIntervalMs = 0;
    double maxFrameIntervalMs = 0;
    double meanDeliveryDelayMs = 0;   // delivery host time − frame host time
    juce::String clockDescription;
};

// AVCaptureSession with the default camera only (audio comes from the AudioEngine).
// Frames go through AVCaptureVideoDataOutput → AVAssetWriter (H.264 .mov), so the
// first frame's PTS is known exactly without the macOS 15.2+ startPTS delegate.
class CameraRecorder
{
public:
    CameraRecorder();
    ~CameraRecorder();

    juce::String start();
    void* previewLayer() const; // AVCaptureVideoPreviewLayer*
    juce::String cameraName() const;
    juce::String clockDescription() const;

    juce::String beginWriting (const juce::File& movieFile, const juce::File& framesCsv);
    void endWriting (std::function<void (CameraTakeResult)> onDoneOnMessageThread);

    struct Impl; // public so the Objective-C delegate can name it

private:
    std::unique_ptr<Impl> impl;
    juce::File framesCsvPath;
};

// Requests camera and microphone access through AVFoundation before CoreAudio opens
// (JUCE's RuntimePermissions is a stub on macOS; ADR 0002 spike finding 4).
void requestCaptureAccess (std::function<void (bool camera, bool microphone)> onDoneOnMessageThread);
