#pragma once

#include "app/LiveEngine.h"
#include "capture/LoopbackRun.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

// `overdub --selftest loopback` (ADR 0005, issue #26): one capture::LoopbackRun on the live
// device (the same run the calibration flow uses, issue #39), reported as JSON.
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
    void fail (const core::Error& error);
    core::Fields baseReport();

    LiveEngine& live;
    Done done;
    std::unique_ptr<capture::LoopbackRun> run;
};
} // namespace overdub::app
