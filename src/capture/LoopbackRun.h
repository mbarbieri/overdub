#pragma once

#include "capture/LoopbackMeasurement.h"
#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "song/SongPackage.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <memory>
#include <optional>

// One loopback latency measurement on the live engine (ADR 0002, ADR 0005), shared by
// `overdub --selftest loopback` and the calibration flow (issue #39): plays the loopback noise
// bursts as a Backing Track, records them back as a Pass on input channel 0 (a cable from output 1
// to input 1, or speakers and microphone) through the real record path, and measures where the
// placed Take put each burst (capture/LoopbackMeasurement.h). Monitoring stays off so speakers do
// not feed back.
//
// The Song lives in a temp package inside `folder`, which the run creates and its destructor
// deletes. The run replaces whatever Song the engine played: the caller detaches its own Song
// first and loads it again afterwards.
//
// Message thread (any one non-audio thread): start, poll and the destructor.
namespace overdub::capture
{
struct LoopbackRunResult
{
    LoopbackMeasurement measurement;
    // loopbackReportFields plus take_origin, reported_latency_samples (input + output + plan
    // latency), calibration_offset_samples (what the Take was placed with), input_latency_samples,
    // output_latency_samples, plan_latency_samples and overrun_frames.
    core::Fields report;
};

class LoopbackRun
{
public:
    using Clock = std::chrono::steady_clock;

    // Builds the run's Song (Backing Track of bursts, one Armed Track on input channel 0), loads it,
    // seeks to 0, starts the Pass and plays; `now` starts the timeout. Logs
    // capture.calibration_run_started. Errors: capture.calibration_folder_failed (path), the package
    // and import Errors, PassRecorder::start's Errors.
    static core::Expected<std::unique_ptr<LoopbackRun>> start (engine::Engine& engine, RecordLatency latency, int sampleRate, juce::File folder, Clock::time_point now = Clock::now());

    // Stops the transport (when the run did not finish), drops the recorder and deletes the folder.
    ~LoopbackRun();

    LoopbackRun (const LoopbackRun&) = delete;
    LoopbackRun& operator= (const LoopbackRun&) = delete;

    // The Song sample at which the run stops: the signal's end plus time for the last burst's round
    // trip.
    SampleCount endSample() const;

    // Call regularly (a timer). Once the transport reaches endSample it asks the Pass to stop; once
    // the audio thread has stopped it, it stops the transport, measures and returns the result
    // (logging capture.calibration_run_finished). None while the run goes on, and after the result.
    // A run not stopped 5 s after it should have ended (the device stopped calling back) asks the
    // Pass to stop and fails with capture.calibration_timeout (elapsed_ms).
    // Errors: PassRecorder::finish's Errors, capture.calibration_take_missing, the Take's read Errors.
    std::optional<core::Expected<LoopbackRunResult>> poll (Clock::time_point now = Clock::now());

private:
    LoopbackRun (engine::Engine& engine, RecordLatency latency, int sampleRate, juce::File folder, Clock::time_point now);

    core::Expected<void> begin();
    core::Expected<LoopbackRunResult> measure (const RecordedPass& pass);

    engine::Engine& engine;
    RecordLatency latency;
    int sampleRate = 0;
    juce::File folder;
    LoopbackSignal signal;
    std::unique_ptr<song::SongPackage> package;
    std::unique_ptr<PassRecorder> recorder;
    Clock::time_point startedAt;
    bool stopRequested = false;
    bool done = false; // a result or an Error was returned
};
} // namespace overdub::capture
