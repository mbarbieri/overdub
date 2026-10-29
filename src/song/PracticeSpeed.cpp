#include "song/PracticeSpeed.h"

#include "core/Log.h"
#include "song/BackingTrackImport.h"
#include "song/TransposeRender.h"

#include <algorithm>
#include <cmath>
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

// The file playing `source` at `key`: its whole render, or else its range render in `window`.
std::optional<juce::File> renderPlaying (const juce::File& folder, const Song& song, const ClipSource& source, PracticeRenderKey key, std::optional<SampleRange> window)
{
    if (const auto whole = practiceRenderFileFor (folder, source, key); whole.existsAsFile())
        return whole;
    if (! window.has_value())
        return std::nullopt;
    const auto range = sourceRangeIn (song, source, *window);
    if (! range.has_value())
        return std::nullopt;
    if (const auto partial = practiceRangeRenderFileFor (folder, source, key, *range); partial.existsAsFile())
        return partial;
    return std::nullopt;
}
} // namespace

SampleRange loopRenderWindow (SampleCount loopStart, SampleCount loopEnd, int sampleRate)
{
    const auto margin = (SampleCount) std::llround (loopRenderMarginSeconds * sampleRate);
    return { std::max<SampleCount> (0, loopStart - margin), loopEnd + margin };
}

std::optional<SampleRange> sourceRangeIn (const Song& song, const ClipSource& source, SampleRange window)
{
    std::optional<SampleRange> range;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.clips)
        {
            const auto from = std::max (clip.timelineStart, window.start);
            const auto to = std::min (clip.timelineStart + clip.length, window.end);
            if (clip.source != source || from >= to)
                continue;
            const SampleRange played { clip.sourceOffset + from - clip.timelineStart, clip.sourceOffset + to - clip.timelineStart };
            range = range.has_value() ? SampleRange { std::min (range->start, played.start), std::max (range->end, played.end) } : played;
        }
    return range;
}

core::Expected<int> checkPracticeSpeed (int speedPercent)
{
    if (speedPercent < minPracticeSpeed || speedPercent > maxPracticeSpeed)
        return std::unexpected (core::Error {
            "song.practice_speed_out_of_range",
            "Practice Speed is a whole percent from 25 to 125",
            { { "speed_percent", speedPercent }, { "min", minPracticeSpeed }, { "max", maxPracticeSpeed } },
        });
    return speedPercent;
}

juce::File practiceRenderFileFor (const juce::File& folder, const ClipSource& source, PracticeRenderKey key)
{
    const auto speed = ".speed" + juce::String (key.speedPercent) + ".wav";
    if (source.kind == ClipSourceKind::take)
        return folder.getChildFile ("take-" + juce::String (source.id) + speed);
    const auto transpose = ".transpose" + juce::String (transposeFileTag (key.transpose));
    return folder.getChildFile ("backing-" + juce::String (source.id) + transpose + speed);
}

juce::File practiceRangeRenderFileFor (const juce::File& folder, const ClipSource& source, PracticeRenderKey key, SampleRange sourceRange)
{
    const auto whole = practiceRenderFileFor (folder, source, key);
    const auto range = ".range" + juce::String (sourceRange.start) + "-" + juce::String (sourceRange.end);
    return folder.getChildFile (whole.getFileNameWithoutExtension() + range + ".wav");
}

std::vector<PracticeRenderJob> missingPracticeRenders (const juce::File& packageRoot, const juce::File& folder, const Song& song, PracticeRenderKey key, std::optional<SampleRange> window)
{
    std::vector<PracticeRenderJob> missing;
    if (key.speedPercent == normalPracticeSpeed)
        return missing;
    for (const auto& source : playedSources (song))
    {
        const auto from = mediaFileFor (packageRoot, song, source);
        if (! from.has_value() || ! from->existsAsFile() || practiceRenderFileFor (folder, source, key).existsAsFile())
            continue;
        const auto transpose = source.kind == ClipSourceKind::backing ? key.transpose : Transpose {};
        if (! window.has_value())
        {
            missing.push_back ({ source, *from, practiceRenderFileFor (folder, source, key), transpose });
            continue;
        }
        const auto range = sourceRangeIn (song, source, *window);
        if (! range.has_value())
            continue;
        if (const auto to = practiceRangeRenderFileFor (folder, source, key, *range); ! to.existsAsFile())
            missing.push_back ({ source, *from, to, transpose, range });
    }
    return missing;
}

std::map<ClipSource, juce::File> practiceRendersOf (const juce::File& folder, const Song& song, PracticeRenderKey key, std::optional<SampleRange> window)
{
    std::map<ClipSource, juce::File> renders;
    if (key.speedPercent == normalPracticeSpeed)
        return renders;
    for (const auto& source : playedSources (song))
        if (const auto file = renderPlaying (folder, song, source, key, window))
            renders.emplace (source, *file);
    return renders;
}

int deletePracticeRendersExcept (const juce::File& folder, const Song& song, PracticeRenderKey key, std::optional<SampleRange> window)
{
    std::set<juce::File> kept;
    for (const auto& [source, file] : practiceRendersOf (folder, song, key, window))
        kept.insert (file);
    int deleted = 0;
    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
        if (! kept.contains (file) && file.deleteFile())
            ++deleted;
    return deleted;
}

//==============================================================================

std::unique_ptr<PracticeSpeedRun> PracticeSpeedRun::start (int sampleRate, PracticeRenderKey key, std::optional<SampleRange> loopWindow, std::vector<PracticeRenderJob> jobs, Clock::time_point now)
{
    std::unique_ptr<PracticeSpeedRun> run (new PracticeSpeedRun (key, loopWindow, std::move (jobs), now));
    auto started = run->fields (now);
    started.erase ("duration_ms");
    core::logEvent (core::Level::info, "song.practice_speed_started", started);
    std::vector<RenderWorker::Render> renders;
    for (const auto& job : run->jobs)
        renders.push_back ([job, sampleRate, speedPercent = key.speedPercent] (std::stop_token stop) -> core::Expected<void> {
            auto rendered = renderAtPracticeSpeed (job.from, job.to, sampleRate, job.transpose, speedPercent, stop, job.sourceRange);
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

PracticeSpeedRun::PracticeSpeedRun (PracticeRenderKey key, std::optional<SampleRange> loopWindow, std::vector<PracticeRenderJob> renderJobs, Clock::time_point now)
    : target (key),
      window (loopWindow),
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
    core::Fields logged { { "speed_percent", target.speedPercent },
                          { "semitones", target.transpose.semitones },
                          { "cents", target.transpose.cents },
                          { "renders", jobs.size() },
                          { "scope", "song" },
                          { "duration_ms", std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count() } };
    // A run's jobs are all range renders of its Loop's window, or none is.
    if (std::ranges::any_of (jobs, [] (const PracticeRenderJob& job) { return job.sourceRange.has_value(); }))
    {
        OVERDUB_CHECK (window.has_value(), "song.practice_range_render_without_loop");
        logged["scope"] = "loop";
        logged["window_start"] = window->start;
        logged["window_end"] = window->end;
    }
    return logged;
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
