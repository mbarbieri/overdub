#include "app/LoopbackSelftest.h"

#include "song/AudioFileDecode.h"

namespace overdub::app
{
namespace
{
// Time after the signal ends before stopping, for the last burst's round trip.
constexpr int tailMs = 750;
} // namespace

LoopbackSelftest::LoopbackSelftest (LiveEngine& liveEngine, Done whenDone)
    : live (liveEngine),
      done (std::move (whenDone)),
      folder (juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("overdub-selftest-loopback", "", false))
{
}

LoopbackSelftest::~LoopbackSelftest()
{
    stopTimer();
    recorder.reset();
    package.reset();
    folder.deleteRecursively();
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
    if (auto* engine = live.engine())
        engine->stop();
    recorder.reset();

    auto report = baseReport();
    report["pass"] = false;
    report["error"] = error.code;
    done (report);
}

void LoopbackSelftest::start()
{
    auto* engine = live.engine();
    if (engine == nullptr)
        return fail (core::Error { "app.selftest_no_device", "no audio device is open", {} });
    if (! folder.createDirectory().wasOk())
        return fail (core::Error { "app.selftest_folder_failed", "could not create the selftest folder", { { "path", folder.getFullPathName().toStdString() } } });

    const auto sampleRate = live.sampleRate();
    if (sampleRate <= 0)
        return fail (core::Error { "app.selftest_no_device", "the audio device reports no sample rate", {} });

    auto created = song::SongPackage::create (folder.getChildFile ("Loopback.overdub"), song::makeEmptySong (sampleRate));
    if (! created.has_value())
        return fail (created.error());
    package = std::move (*created);

    // The bursts reach the Song as an imported Backing Track, like any audio the user plays along to.
    signal = capture::makeLoopbackSignal (sampleRate);
    const auto signalFile = folder.getChildFile ("Loopback Bursts.wav");
    if (auto written = song::writeFloatWavAtomically (signalFile, signal.audio, sampleRate, {}); ! written.has_value())
        return fail (written.error());
    if (auto imported = package->importBackingTrack (signalFile); ! imported.has_value())
        return fail (imported.error());

    auto song = package->song();
    song::Track loopbackTrack;
    loopbackTrack.id = song::makeId();
    loopbackTrack.name = "Loopback";
    loopbackTrack.armed = true;
    loopbackTrack.inputChannels = { 0 };
    song.tracks.push_back (loopbackTrack);
    if (auto edited = package->edit (song, "Add Loopback Track"); ! edited.has_value())
        return fail (edited.error());

    engine->stop();
    engine->setMonitoring (false);
    engine->loadSong (package->song(), package->location());
    engine->seek (0);

    auto started = capture::PassRecorder::start (*package, *engine, live.recordLatency());
    if (! started.has_value())
        return fail (started.error());
    recorder = std::move (*started);
    engine->play();

    core::logEvent (core::Level::info, "app.selftest_started", { { "selftest", "loopback" }, { "bursts", signal.burstStarts.size() } });
    startTimer ((int) (1000.0 * signal.audio.getNumSamples() / sampleRate) + tailMs);
}

void LoopbackSelftest::timerCallback()
{
    stopTimer();
    finish();
}

void LoopbackSelftest::finish()
{
    auto* engine = live.engine();
    auto pass = recorder->stop();
    engine->stop();
    recorder.reset();
    if (! pass.has_value())
        return fail (pass.error());

    const auto& takes = package->song().takes;
    const auto take = std::ranges::find (takes, pass->takeIds.front(), &song::Take::id);
    if (take == takes.end())
        return fail (core::Error { "app.selftest_take_missing", "the recorded Take is not in the Song", {} });
    const auto audio = song::readWavFile (package->location().getChildFile (take->file));
    if (! audio.has_value())
        return fail (audio.error());

    const auto maxLag = (int) (signal.audio.getNumSamples() / 24); // ±0.25 s
    const auto measurement = capture::measureLoopback (signal, 0, audio->getReadPointer (0), audio->getNumSamples(), take->origin, maxLag);

    auto report = baseReport();
    for (const auto& [key, value] : capture::loopbackReportFields (measurement).items())
        report[key] = value;
    report["take_origin"] = take->origin;
    report["reported_latency_samples"] = take->compensation.reportedLatencySamples;
    report["overrun_frames"] = pass->overrunFrames;
    done (report);
}
} // namespace overdub::app
