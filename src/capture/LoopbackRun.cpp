#include "capture/LoopbackRun.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"

#include <algorithm>

namespace overdub::capture
{
namespace
{
// Time after the signal ends before stopping, for the last burst's round trip.
constexpr double tailSeconds = 0.75;
// How long past its expected end a run waits for the audio thread before giving up.
constexpr auto timeoutMargin = std::chrono::seconds (5);
} // namespace

LoopbackRun::LoopbackRun (engine::Engine& engineToUse, RecordLatency recordLatency, int rate, juce::File runFolder, Clock::time_point now)
    : engine (engineToUse), latency (recordLatency), sampleRate (rate), folder (std::move (runFolder)), startedAt (now)
{
}

LoopbackRun::~LoopbackRun()
{
    if (recorder != nullptr)
        engine.stop();
    recorder.reset();
    package.reset();
    folder.deleteRecursively();
}

core::Expected<std::unique_ptr<LoopbackRun>> LoopbackRun::start (engine::Engine& engine, RecordLatency latency, int sampleRate, juce::File folder, Clock::time_point now)
{
    std::unique_ptr<LoopbackRun> run { new LoopbackRun (engine, latency, sampleRate, std::move (folder), now) };
    if (auto begun = run->begin(); ! begun.has_value())
        return std::unexpected (begun.error());
    return run;
}

core::Expected<void> LoopbackRun::begin()
{
    folder.deleteRecursively();
    if (! folder.createDirectory().wasOk())
        return std::unexpected (core::Error { "capture.calibration_folder_failed", "could not create the calibration folder", { { "path", folder.getFullPathName().toStdString() } } });

    auto created = song::SongPackage::create (folder.getChildFile ("Loopback.overdub"), song::makeEmptySong (sampleRate));
    if (! created.has_value())
        return std::unexpected (created.error());
    package = std::move (*created);

    // The bursts reach the Song as an imported Backing Track, like any audio the user plays along to.
    signal = makeLoopbackSignal (sampleRate);
    const auto signalFile = folder.getChildFile ("Loopback Bursts.wav");
    if (auto written = song::writeFloatWavAtomically (signalFile, signal.audio, sampleRate, {}); ! written.has_value())
        return std::unexpected (written.error());
    if (auto imported = package->importBackingTrack (signalFile); ! imported.has_value())
        return std::unexpected (imported.error());

    auto song = package->song();
    song::Track loopbackTrack;
    loopbackTrack.id = song::makeId();
    loopbackTrack.name = "Loopback";
    loopbackTrack.armed = true;
    loopbackTrack.inputChannels = { 0 };
    song.tracks.push_back (loopbackTrack);
    if (auto edited = package->edit (song, "Add Loopback Track"); ! edited.has_value())
        return std::unexpected (edited.error());

    engine.stop();
    engine.setMonitoring (false);
    engine.loadSong (package->song(), package->location());
    engine.seek (0);

    // The whole run fits in the capture FIFO, so a slow io thread can never overrun it.
    PassRecorderOptions options;
    options.fifoSeconds = (double) endSample() / sampleRate + 1.0;
    auto started = PassRecorder::start (*package, engine, latency, options);
    if (! started.has_value())
        return std::unexpected (started.error());
    recorder = std::move (*started);
    engine.play();

    core::logEvent (core::Level::info,
                    "capture.calibration_run_started",
                    { { "bursts", signal.burstStarts.size() }, { "sample_rate", sampleRate }, { "end_sample", endSample() }, { "calibration_offset_samples", latency.calibrationOffsetSamples } });
    return {};
}

SampleCount LoopbackRun::endSample() const
{
    return signal.audio.getNumSamples() + (SampleCount) (tailSeconds * sampleRate);
}

std::optional<core::Expected<LoopbackRunResult>> LoopbackRun::poll (Clock::time_point now)
{
    if (recorder == nullptr || done)
        return std::nullopt;

    const auto deadline = startedAt + std::chrono::milliseconds ((std::int64_t) (1000.0 * (double) endSample() / sampleRate)) + timeoutMargin;
    const auto timedOut = now >= deadline;
    if (! stopRequested && (timedOut || engine.position() >= endSample()))
    {
        recorder->requestStop();
        stopRequested = true;
    }
    if (! recorder->hasStopped())
    {
        if (! timedOut)
            return std::nullopt;
        // The recorder stays: the destructor closes the Pass if the audio thread comes back.
        done = true;
        engine.stop();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
        return std::unexpected (core::Error { "capture.calibration_timeout", "the audio device stopped during the calibration run", { { "elapsed_ms", elapsedMs } } });
    }

    done = true;
    engine.stop();
    auto pass = recorder->finish();
    recorder.reset();
    if (! pass.has_value())
        return std::unexpected (pass.error());
    return measure (*pass);
}

core::Expected<LoopbackRunResult> LoopbackRun::measure (const RecordedPass& pass)
{
    const auto& takes = package->song().takes;
    const auto take = std::ranges::find (takes, pass.takeIds.front(), &song::Take::id);
    if (take == takes.end())
        return std::unexpected (core::Error { "capture.calibration_take_missing", "the recorded Take is not in the Song", { { "pass_id", pass.passId } } });
    const auto audio = song::readWavFile (package->location().getChildFile (take->file));
    if (! audio.has_value())
        return std::unexpected (audio.error());

    const auto maxLag = (int) (signal.audio.getNumSamples() / 24); // ±0.25 s
    LoopbackRunResult result;
    result.measurement = measureLoopback (signal, 0, audio->getReadPointer (0), audio->getNumSamples(), take->origin, maxLag);
    result.report = loopbackReportFields (result.measurement);
    result.report["take_origin"] = take->origin;
    result.report["reported_latency_samples"] = take->compensation.reportedLatencySamples;
    result.report["calibration_offset_samples"] = take->compensation.calibrationOffsetSamples;
    result.report["input_latency_samples"] = latency.inputLatencySamples;
    result.report["output_latency_samples"] = latency.outputLatencySamples;
    result.report["plan_latency_samples"] = engine.latencySamples();
    result.report["overrun_frames"] = pass.overrunFrames;

    core::logEvent (core::Level::info, "capture.calibration_run_finished", result.report);
    return result;
}
} // namespace overdub::capture
