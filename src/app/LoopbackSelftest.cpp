#include "app/LoopbackSelftest.h"

namespace overdub::app
{
namespace
{
constexpr int pollIntervalMs = 50;
} // namespace

LoopbackSelftest::LoopbackSelftest (LiveEngine& liveEngine, Done whenDone)
    : live (liveEngine), done (std::move (whenDone))
{
}

LoopbackSelftest::~LoopbackSelftest()
{
    stopTimer();
    run.reset();
}

core::Fields LoopbackSelftest::baseReport()
{
    auto report = live.deviceFields();
    report["selftest"] = "loopback";
    const auto latency = live.recordLatency();
    report["input_latency_samples"] = latency.inputLatencySamples;
    report["output_latency_samples"] = latency.outputLatencySamples;
    report["calibration_offset_samples"] = latency.calibrationOffsetSamples;
    if (auto* engine = live.engine())
        report["plan_latency_samples"] = engine->latencySamples();
    return report;
}

void LoopbackSelftest::fail (const core::Error& error)
{
    core::logError (error);
    stopTimer();
    run.reset();

    auto report = baseReport();
    report["pass"] = false;
    report["error"] = error.code;
    done (report);
}

void LoopbackSelftest::start()
{
    auto* engine = live.engine();
    if (engine == nullptr || live.sampleRate() <= 0)
        return fail (core::Error { "app.selftest_no_device", "no audio device is open", {} });

    // Unique per run, not the next unused name: the run deletes its folder at start and end, and a
    // second process picking the same name would delete this one's (issue #187).
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-selftest-loopback-" + juce::Uuid().toString());
    auto started = capture::LoopbackRun::start (*engine, live.recordLatency(), live.sampleRate(), folder);
    if (! started.has_value())
        return fail (started.error());
    run = std::move (*started);
    core::logEvent (core::Level::info, "app.selftest_started", { { "selftest", "loopback" } });
    startTimer (pollIntervalMs);
}

void LoopbackSelftest::timerCallback()
{
    auto polled = run->poll();
    if (! polled.has_value())
        return;
    stopTimer();
    run.reset();
    if (! polled->has_value())
        return fail (polled->error());

    auto report = baseReport();
    for (const auto& [key, value] : (*polled)->report.items())
        report[key] = value;
    done (report);
}
} // namespace overdub::app
