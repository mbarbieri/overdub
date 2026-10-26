#include "ui/ExportRun.h"

#include "core/Log.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
core::Error exportPluginCrashed (const juce::File& destination)
{
    return core::Error { "ui.export_plugin_crashed", "a plugin crashed during the export", { { "path", destination.getFullPathName().toStdString() } } };
}
} // namespace

ExportRun::ExportRun (exporting::ExportRequest exportRequest,
                      std::optional<exporting::MovieSource> movieSource,
                      const song::SongPackage& songPackage,
                      plugin::InsertHost& insertHost,
                      int renderBlockSize,
                      Clock::time_point now)
    : package (songPackage),
      inserts (insertHost),
      blockSize (renderBlockSize),
      requestedAt (now),
      request (std::move (exportRequest)),
      movie (std::move (movieSource))
{
}

ExportRun::~ExportRun() = default;

ExportPhase ExportRun::phase() const
{
    return job != nullptr ? ExportPhase::rendering : ExportPhase::waitingForAudio;
}

double ExportRun::progress() const
{
    return job != nullptr ? job->progress() : 0.0;
}

bool ExportRun::hasWrittenFile() const
{
    return job != nullptr && job->isDone();
}

std::optional<ExportEnd> ExportRun::advance (Clock::time_point now, bool audioLetGo)
{
    if (job == nullptr)
    {
        if (auto end = startRenderOnceAudioLetGo (audioLetGo))
            return end;
        return giveUpIfAudioUnresponsive (now);
    }
    if (! job->isDone())
        return std::nullopt;

    auto result = job->takeResult();
    job.reset();
    if (! OVERDUB_CHECK (result.has_value(), "ui.export_result_missing"))
        return ExportEnd {};
    if (! result->has_value())
        return ExportEnd { .error = result->error() };
    return ExportEnd { .file = (*result)->file };
}

std::optional<ExportEnd> ExportRun::startRenderOnceAudioLetGo (bool audioLetGo)
{
    if (! audioLetGo)
        return std::nullopt;
    const auto& song = package.song();
    // A slot that crashed while the export waited (bypassed then, played now) is not reloaded
    // during an export: rendering would pass it through dry. One that crashed for good stays out.
    const auto played = inserts.playedSlots (song);
    const auto missesCrashed = std::ranges::any_of (played, [this] (const song::Id& slotId) {
        return inserts.status (slotId) == plugin::InsertStatus::crashed && ! inserts.crashedForGood (slotId);
    });
    if (missesCrashed)
    {
        auto error = exportPluginCrashed (request.destination);
        stop ("plugin_crashed");
        return ExportEnd { .error = std::move (error) };
    }
    // The audio thread renders the empty plan now: the export may run the plugins.
    inserts.sync (song);
    renderedSlots = inserts.renderedSlots (song);
    auto render = std::make_unique<exporting::MixRender> (song, package.location(), blockSize, inserts.insertFactory());
    if (movie.has_value())
        job = std::make_unique<exporting::ExportJob> (std::move (render), request, std::move (*movie));
    else
        job = std::make_unique<exporting::ExportJob> (std::move (render), request);
    movie.reset();
    return std::nullopt;
}

std::optional<ExportEnd> ExportRun::giveUpIfAudioUnresponsive (Clock::time_point now)
{
    const auto waited = now - requestedAt;
    if (waited < audioTimeout)
        return std::nullopt;
    // The audio thread may still render the plan that holds the plugins: they are not taken over.
    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds> (waited).count();
    core::Error error { "ui.export_audio_unresponsive",
                        "the audio device stopped calling back before the export could start",
                        { { "path", request.destination.getFullPathName().toStdString() }, { "waited_ms", waitedMs } } };
    stop ("audio_unresponsive");
    return ExportEnd { .error = std::move (error) };
}

std::optional<ExportEnd> ExportRun::crashed (const std::vector<plugin::InsertCrash>& crashes)
{
    if (! std::ranges::any_of (crashes, [this] (const plugin::InsertCrash& crash) { return renders (crash.slotId); }))
        return std::nullopt;

    // The export's plan passes the dead plugin's audio through dry: its file would be wrong.
    auto error = exportPluginCrashed (request.destination);
    if (hasWrittenFile())
    {
        // A job that already finished has written its file: that file may hold the crash, so it goes.
        if (auto result = job->takeResult(); result.has_value() && result->has_value())
            (*result)->file.deleteFile();
        core::logEvent (core::Level::info, "ui.export_stopped", { { "reason", "plugin_crashed" }, { "phase", "finished" } });
        job.reset();
    }
    else
    {
        stop ("plugin_crashed");
    }
    return ExportEnd { .error = std::move (error) };
}

bool ExportRun::renders (const song::Id& slotId) const
{
    // While it waits for the audio, its render will be built from the Song as it is then.
    if (job == nullptr)
        return inserts.playedSlots (package.song()).contains (slotId);
    return renderedSlots.contains (slotId);
}

void ExportRun::followSong()
{
    // A slot an edit removed stays hosted: the export's plan may render it, and only a hosted
    // plugin's crash is heard.
    inserts.sync (package.song(), plugin::RemovedSlots::keep);
}

ExportEnd ExportRun::cancel()
{
    const auto path = request.destination.getFullPathName().toStdString();
    const auto phase = phaseName();
    if (job != nullptr)
    {
        job->stop();
        auto result = job->takeResult();
        job.reset();
        if (result.has_value() && result->has_value())
            return ExportEnd { .file = (*result)->file };
        // A render that failed before it heard the stop ends with its own Error.
        if (result.has_value() && result->error().code != "export.cancelled")
            return ExportEnd { .error = result->error() };
    }
    core::logEvent (core::Level::info, "ui.export_cancelled", { { "path", path }, { "phase", phase } });
    return ExportEnd { .error = core::Error { "ui.export_cancelled", "the export was cancelled", { { "path", path } } } };
}

void ExportRun::stop (const char* reason)
{
    core::logEvent (core::Level::info, "ui.export_stopped", { { "reason", reason }, { "phase", phaseName() } });
    job.reset(); // stops the worker and waits for it
}

const char* ExportRun::phaseName() const
{
    return job != nullptr ? "rendering" : "waiting_for_audio";
}
} // namespace overdub::ui
