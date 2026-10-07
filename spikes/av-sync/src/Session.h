// PROTOTYPE — issue #7 spike.
#pragma once
#include "ClockMap.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

// One recording pass, on disk:
//   session.json          this struct
//   take.wav              dry mono Take; sample 0 sits at takeStartSample on the song timeline
//   video.mov             Video Take; movie time 0 = first frame
//   anchors-record.csv    ClockAnchor per audio callback while recording
//   frames.csv            per video frame: capture-clock PTS, host time, delivery time
//   playback-*.csv, export-*.mp4, mix.wav, analysis/   written by later steps
struct SessionInfo
{
    juce::File dir;

    double sampleRate = 0;
    int bufferSize = 0;
    int inputLatencySamples = 0;   // as reported by JUCE
    int outputLatencySamples = 0;  // as reported by JUCE
    int rtCalibrationSamples = 0;  // subtracted from the reported round trip (ADR 0002 loopback offset)

    int64_t recordStartSample = 0; // transport sample of the first recorded input block
    int64_t takeStartSample = 0;   // song sample of take sample 0
    int64_t takeLengthSamples = 0;

    ClockFit clock;                // fit of anchors-record.csv
    double driftAtEndMs = 0;       // naive minus fitted mapping, at the last video frame

    juce::String cameraName;
    juce::String captureClock;     // description of AVCaptureSession.synchronizationClock
    double captureClockRelativeRate = 1.0;
    int64_t videoFirstPtsNs = 0;   // capture-clock PTS of movie time 0
    int64_t videoFirstHostNs = 0;  // same instant in host time
    int videoFrameCount = 0;
    int videoDroppedFrames = 0;
    double videoDurationSeconds = 0;
    double cameraCalibrationMs = 0; // per-camera latency; subtracted from the video offset

    // Song sample at which movie time 0 plays. Fractional; the export rounds it to the sample.
    double videoOffsetSamples = 0;

    // Movie seconds per song sample: nsPerSample / 1e9, i.e. corrected for audio-clock drift.
    double movieSecondsPerSongSample() const { return clock.nsPerSample / 1.0e9; }
    int64_t songLengthSamples() const { return takeStartSample + takeLengthSamples; }

    juce::File file (const juce::String& name) const { return dir.getChildFile (name); }
};

juce::File sessionsRoot();
juce::File createSessionDirectory();

bool saveSession (const SessionInfo& session);
bool loadSession (const juce::File& dir, SessionInfo& session, juce::String& error);

bool writeAnchorsCsv (const juce::File& file, const std::vector<ClockAnchor>& anchors);

// Song-timeline mono buffer: silence, then the Take at takeStartSample.
bool buildSongMix (const SessionInfo& session, juce::AudioBuffer<float>& mix, juce::String& error);
bool writeMonoWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate);
