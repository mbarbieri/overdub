#pragma once

#include "app/LiveEngine.h"
#include "capture/LoopbackMeasurement.h"
#include "capture/PassRecorder.h"
#include "song/SongPackage.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

// `overdub --selftest loopback` (ADR 0005, issue #26): plays the loopback noise bursts as a Backing
// Track on the live device, records them back as a Pass on input channel 0 (a cable from output 1
// to input 1, or the built-in speakers and microphone) through the real record path, and measures
// where the placed Take put each burst (capture/LoopbackMeasurement.h). The Song lives in a temp
// package that is deleted afterwards. Monitoring stays off so speakers do not feed back.
//
// The report is one JSON object: "selftest": "loopback", "pass", the device fields, the reported
// latencies and calibration offset the Take was placed with, the plan latency, and the
// measurement fields (offset_samples: how late Takes land; jitter_samples against
// jitter_limit_samples). On a failure before measuring, "pass" is false and "error" holds the code.
namespace overdub::app
{
class LoopbackSelftest final : private juce::Timer
{
public:
    using Done = std::function<void (const core::Fields& report)>;

    // Message thread. The live engine must outlive the selftest.
    LoopbackSelftest (LiveEngine& liveEngine, Done done);
    ~LoopbackSelftest() override;

    // Starts playing and recording; calls `done` once, on the message thread, when finished.
    void start();

private:
    void timerCallback() override;
    void finish();
    void fail (const core::Error& error);
    core::Fields baseReport();

    LiveEngine& live;
    Done done;
    juce::File folder;
    capture::LoopbackSignal signal;
    std::unique_ptr<song::SongPackage> package;
    std::unique_ptr<capture::PassRecorder> recorder;
};
} // namespace overdub::app
