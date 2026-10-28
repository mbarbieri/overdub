#include "song/PracticeSpeed.h"

#include "core/Log.h"
#include "song/BackingTrackImport.h"
#include "song/TransposeRender.h"

#include <algorithm>
#include <set>

namespace overdub::song
{
namespace
{
// The distinct sources some Clip plays, in Track and Clip order.
std::vector<ClipSource> playedSources (const Song& song)
{
    std::vector<ClipSource> sources;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
            if (! std::ranges::contains (sources, clip.source))
                sources.push_back (clip.source);
    return sources;
}
} // namespace

core::Expected<int> checkPracticeSpeed (int speedPercent)
{
    if (speedPercent < minPracticeSpeed || speedPercent > maxPracticeSpeed)
        return std::unexpected (core::Error {
            "song.practice_speed_out_of_range",
            "Practice Speed is a whole percent from 50 to 125",
            { { "speed_percent", speedPercent }, { "min", minPracticeSpeed }, { "max", maxPracticeSpeed } },
        });
    return speedPercent;
}

juce::File practiceRenderFileFor (const juce::File& folder, const ClipSource& source, PracticeRenderKey key)
{
    const auto speed = ".speed" + juce::String (key.speedPercent) + ".wav";
    if (source.kind == ClipSourceKind::take)
        return folder.getChildFile ("take-" + juce::String (source.id) + speed);
    const auto transpose = ".transpose" + juce::String (key.semitones >= 0 ? "+" : "") + juce::String (key.semitones);
    return folder.getChildFile ("backing-" + juce::String (source.id) + transpose + speed);
}

std::vector<PracticeRenderJob> missingPracticeRenders (const juce::File& packageRoot, const juce::File& folder, const Song& song, PracticeRenderKey key)
{
    std::vector<PracticeRenderJob> missing;
    if (key.speedPercent == normalPracticeSpeed)
        return missing;
    for (const auto& source : playedSources (song))
    {
        const auto from = mediaFileFor (packageRoot, song, source);
        const auto to = practiceRenderFileFor (folder, source, key);
        if (from.has_value() && from->existsAsFile() && ! to.existsAsFile())
            missing.push_back ({ source, *from, to, source.kind == ClipSourceKind::backing ? key.semitones : 0 });
    }
    return missing;
}

std::map<ClipSource, juce::File> practiceRendersOf (const juce::File& folder, const Song& song, PracticeRenderKey key)
{
    std::map<ClipSource, juce::File> renders;
    if (key.speedPercent == normalPracticeSpeed)
        return renders;
    for (const auto& source : playedSources (song))
        if (const auto file = practiceRenderFileFor (folder, source, key); file.existsAsFile())
            renders.emplace (source, file);
    return renders;
}

int deletePracticeRendersExcept (const juce::File& folder, const Song& song, PracticeRenderKey key)
{
    std::set<juce::File> kept;
    for (const auto& [source, file] : practiceRendersOf (folder, song, key))
        kept.insert (file);
    int deleted = 0;
    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
        if (! kept.contains (file) && file.deleteFile())
            ++deleted;
    return deleted;
}

//==============================================================================

std::unique_ptr<PracticeSpeedRun> PracticeSpeedRun::start (int sampleRate, PracticeRenderKey key, std::vector<PracticeRenderJob> jobs, Clock::time_point now)
{
    std::unique_ptr<PracticeSpeedRun> run (new PracticeSpeedRun (key, std::move (jobs), now));
    auto started = run->fields (now);
    started.erase ("duration_ms");
    core::logEvent (core::Level::info, "song.practice_speed_started", started);
    std::vector<RenderWorker::Render> renders;
    for (const auto& job : run->jobs)
        renders.push_back ([job, sampleRate, speedPercent = key.speedPercent] (std::stop_token stop) -> core::Expected<void> {
            auto rendered = renderAtPracticeSpeed (job.from, job.to, sampleRate, job.semitones, speedPercent, stop);
            if (! rendered.has_value())
            {
                rendered.error().fields["source_kind"] = toString (job.source.kind);
                rendered.error().fields["source_id"] = job.source.id;
            }
            return rendered;
        });
    run->worker = std::make_unique<RenderWorker> (std::move (renders));
    return run;
}

PracticeSpeedRun::PracticeSpeedRun (PracticeRenderKey key, std::vector<PracticeRenderJob> renderJobs, Clock::time_point now)
    : target (key),
      jobs (std::move (renderJobs)),
      startedAt (now)
{
}

PracticeSpeedRun::~PracticeSpeedRun()
{
    cancel (Clock::now());
}

core::Fields PracticeSpeedRun::fields (Clock::time_point now) const
{
    return { { "speed_percent", target.speedPercent },
             { "semitones", target.semitones },
             { "renders", jobs.size() },
             { "duration_ms", std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count() } };
}

std::optional<core::Expected<void>> PracticeSpeedRun::advance (Clock::time_point now)
{
    auto ended = worker->take();
    if (ended.has_value() && ended->has_value())
        core::logEvent (core::Level::info, "song.practice_speed_rendered", fields (now));
    failed = ended.has_value() && ! ended->has_value();
    return ended;
}

std::optional<ClipSource> PracticeSpeedRun::failedSource() const
{
    // The worker stops at the first failed render: the jobs before it were all written.
    if (! failed)
        return std::nullopt;
    return jobs.at ((std::size_t) worker->rendersDone()).source;
}

void PracticeSpeedRun::cancel (Clock::time_point now)
{
    if (! worker->stop())
        return;
    auto logged = fields (now);
    logged["renders_done"] = worker->rendersDone();
    core::logEvent (core::Level::info, "song.practice_speed_cancelled", logged);
}
} // namespace overdub::song
