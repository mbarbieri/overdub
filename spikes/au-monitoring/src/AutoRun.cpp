// PROTOTYPE — issue #6 spike.
#include "AutoRun.h"
#include "LoopbackTest.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace juce;

AutoRun::AutoRun (SpikeEngine& e, const StringArray& a) : engine (e), args (a)
{
    resultsFile = spikelog::logDirectory().getChildFile ("results-" + Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".txt");
}

String AutoRun::argValue (const String& flag, const String& fallback) const
{
    const auto index = args.indexOf (flag);
    return index >= 0 && index + 1 < args.size() ? args[index + 1] : fallback;
}

void AutoRun::writeResult (const String& line)
{
    resultsFile.appendText (line + "\n");
    spikelog::line ("result", { { "line", line } });
}

void AutoRun::start()
{
    writeResult ("# auto run args: " + args.joinIntoString (" "));
    addPluginSteps();

    for (auto& size : StringArray::fromTokens (argValue ("--buffers", "64,128,256"), ",", ""))
        addBufferSteps (size.getIntValue());

    if (args.contains ("--editors"))
        addEditorSteps();

    steps.push_back ([this] (auto next)
    {
        writeResult ("# hosting_processes=" + SpikeEngine::countHostingProcesses());
        spikelog::line ("auto_done", { { "results", resultsFile.getFullPathName() } });
        if (args.contains ("--quit"))
            JUCEApplication::getInstance()->systemRequestedQuit();
        next();
    });

    runNext();
}

void AutoRun::runNext()
{
    if (nextStep >= steps.size())
        return;

    auto step = steps[nextStep++];
    step ([this] { MessageManager::callAsync ([this] { runNext(); }); });
}

void AutoRun::addPluginSteps()
{
    const bool oop = args.contains ("--oop");

    for (int t = 0; t < SpikeEngine::numTracks; ++t)
    {
        const auto name = argValue ("--plugin" + String (t + 1), {});
        if (name.isEmpty())
            continue;

        steps.push_back ([this, t, name, oop] (auto next)
        {
            engine.loadPlugin (t, name, oop, [this, t, next] (String error)
            {
                auto fields = engine.describeTrack (t);
                String line = "PLUGIN track=" + String (t) + (error.isEmpty() ? "" : " error=" + error.quoted());
                for (auto& key : fields.getAllKeys())
                    line << " " << key << "=" << fields[key].replaceCharacter (' ', '_');
                writeResult (line);
                next();
            });
        });
    }
}

void AutoRun::addBufferSteps (int bufferSize)
{
    const int cpuSeconds = argValue ("--cpu-seconds", "5").getIntValue();

    steps.push_back ([this, bufferSize] (auto next)
    {
        engine.setBufferSize (bufferSize);
        Timer::callAfterDelay (1500, next);   // let the device settle
    });

    auto loopback = std::make_shared<LoopbackResult>();
    steps.push_back ([this, loopback] (auto next)
    {
        runLoopbackTest (engine, [loopback, next] (LoopbackResult r) { *loopback = r; next(); });
    });

    // CPU phase: plugins processing, monitoring muted (speakers + mic would feed back),
    // both tracks recording silence-backed takes to disk.
    steps.push_back ([this, bufferSize, cpuSeconds, loopback] (auto next)
    {
        engine.setBackingBuffer (AudioBuffer<float> (2, 0));
        const bool armed[] = { true, true };
        const auto xrunsBefore = engine.getHealth().xruns;
        engine.resetMaxCallback();
        engine.start (armed);

        struct Samples { double sum = 0, max = 0; int count = 0; };
        auto samples = std::make_shared<Samples>();
        const int ticks = cpuSeconds * 10;

        for (int i = 1; i <= ticks; ++i)
        {
            Timer::callAfterDelay (i * 100, [this, i, ticks, samples, xrunsBefore, bufferSize, loopback, next]
            {
                const auto health = engine.getHealth();
                samples->sum += health.cpu;
                samples->max = jmax (samples->max, health.cpu);
                samples->count++;

                if (i != ticks)
                    return;

                engine.stop();
                const auto latency = engine.getLatency();
                const auto ms = [&latency] (int s) { return String (1000.0 * s / latency.sampleRate, 3); };
                String line;
                line << "RESULT buffer_req=" << bufferSize
                     << " buffer=" << latency.bufferSize
                     << " sr=" << latency.sampleRate
                     << " in_lat=" << latency.input
                     << " out_lat=" << latency.output
                     << " graph_lat=" << latency.graph
                     << " reported_rt=" << loopback->expected << " (" << ms (loopback->expected) << "ms)"
                     << " measured_rt=" << loopback->measuredMedian << " (" << ms (loopback->measuredMedian) << "ms)"
                     << " align_error=" << loopback->errorSamples << " (" << ms (loopback->errorSamples) << "ms)"
                     << " spread=" << loopback->spreadSamples
                     << " signal=" << (loopback->signalFound ? "yes" : "NO")
                     << " peak_corr=" << String (loopback->peakCorrelation, 3)
                     << " cpu_avg=" << String (100.0 * samples->sum / jmax (1, samples->count), 1) << "%"
                     << " cpu_max=" << String (100.0 * samples->max, 1) << "%"
                     << " max_callback_ms=" << String (health.maxCallbackMs, 3)
                     << " budget_ms=" << String (health.budgetMs, 3)
                     << " xruns=" << (health.xruns - xrunsBefore)
                     << " dropped=" << loopback->dropped;
                writeResult (line);
                next();
            });
        }
    });
}

void AutoRun::addEditorSteps()
{
    for (int t = 0; t < SpikeEngine::numTracks; ++t)
    {
        steps.push_back ([this, t] (auto next)
        {
            auto* plugin = engine.getPlugin (t);
            if (plugin == nullptr)
            {
                next();
                return;
            }

            auto* window = windows.add (new PluginWindow (*plugin, [] {}));
            // AUv3 / remote views arrive asynchronously; measure after they have had time to attach.
            Timer::callAfterDelay (3000, [this, t, window, next]
            {
                auto fields = window->describe();
                writeResult ("EDITOR track=" + String (t) + " kind=" + fields["kind"] + " w=" + fields["w"] + " h=" + fields["h"]);
                windows.removeObject (window);
                next();
            });
        });
    }
}
