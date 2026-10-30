#include "ui/ListeningRenders.h"

#include "core/Log.h"
#include "song/TranscribeMix.h"
#include "song/Transpose.h"

#include <algorithm>
#include <ranges>

namespace overdub::ui
{
namespace
{
// The run renders a Backing Track the Song no longer has (an undone import or Separation).
bool rendersRemovedBackingTrack (const song::TransposeRun& run, const song::Song& song)
{
    return std::ranges::any_of (run.renderJobs(), [&song] (const song::TransposeRenderJob& job) { return ! std::ranges::contains (song.backingTracks, job.backingTrackId, &song::BackingTrack::id); });
}

// The Song with only the octave-shifted Track: the sources its renders are of.
song::Song octaveShiftedSong (const song::Song& song, const song::Id& trackId)
{
    auto shifted = song;
    std::erase_if (shifted.tracks, [&trackId] (const song::Track& track) { return track.id != trackId; });
    return shifted;
}
} // namespace

ListeningRenders::ListeningRenders (juce::File root, const song::Song& song, std::function<void()> playHeardChange)
    : packageRoot (std::move (root)),
      playHeard (std::move (playHeardChange)),
      heardShift (song::missingTransposeRenders (packageRoot, song).empty() ? song.transpose : song::Transpose {}),
      practiceRoot (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-practice-" + juce::Uuid().toString()))
{
    song::deleteTransposeRendersExcept (packageRoot, song.transpose);
}

ListeningRenders::~ListeningRenders()
{
    octaveRun.reset();
    practiceRun.reset();
    practiceRoot.deleteRecursively();
}

std::vector<core::Error> ListeningRenders::advance (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now)
{
    std::vector<core::Error> errors;
    if (auto error = advanceTranspose (song, now))
        errors.push_back (std::move (*error));
    if (auto error = advancePracticeSpeed (song, wanted, now))
        errors.push_back (std::move (*error));
    if (auto error = advanceOctaveShift (song, wanted, now))
        errors.push_back (std::move (*error));
    return errors;
}

void ListeningRenders::follow (const song::Song& song, const ListeningWanted& wanted)
{
    // Only a run that ends fails: here none runs, or the advance cancels it.
    const auto now = Clock::now();
    if (transposeRun == nullptr || rendersRemovedBackingTrack (*transposeRun, song))
    {
        const auto failure = advanceTranspose (song, now);
        OVERDUB_CHECK (! failure.has_value(), "ui.transpose_failed_without_run");
    }
    if (practiceRun == nullptr || ! practiceRunIsWanted (song, wanted))
    {
        const auto failure = advancePracticeSpeed (song, wanted, now);
        OVERDUB_CHECK (! failure.has_value(), "ui.practice_speed_failed_without_run");
    }
    if (octaveRun == nullptr || ! octaveRunIsWanted (song, wanted))
    {
        const auto failure = advanceOctaveShift (song, wanted, now);
        OVERDUB_CHECK (! failure.has_value(), "ui.octave_shift_failed_without_run");
    }
}

engine::PracticePlayback ListeningRenders::practicePlayback (const song::Song& song, const ListeningWanted& wanted) const
{
    return {
        .speedPercent = heardPractice.speedPercent,
        .renders = song::practiceRendersOf (practiceRoot, song, heardPractice, practiceLoopWindow (song, wanted)),
        .octaveShifted = heardOctaveShift (song, wanted),
    };
}

std::optional<core::Error> ListeningRenders::advanceTranspose (const song::Song& song, Clock::time_point now)
{
    const auto target = song.transpose;
    failedTranspose.forgetUnless (target);
    std::optional<core::Error> failure;
    if (transposeRun != nullptr)
    {
        if (transposeRun->transpose() != target || rendersRemovedBackingTrack (*transposeRun, song))
        {
            transposeRun->cancel (now);
            transposeRun.reset();
            sweptBackingTracks.reset();
        }
        else if (auto ended = transposeRun->advance (now))
        {
            transposeRun.reset();
            sweptBackingTracks.reset(); // it may have rendered a Backing Track an undo removed since
            if (! ended->has_value())
            {
                failure = ended->error();
                failedTranspose.add (target, failure->fields.value ("backing_track_id", std::string()));
            }
            else if (heardShift == target)
                playHeard(); // Backing Tracks added since: their renders play now
        }
        else
            return std::nullopt;
    }

    auto backingTrackIds = song.backingTracks | std::views::transform (&song::BackingTrack::id) | std::ranges::to<std::vector>();
    if (backingTrackIds != sweptBackingTracks)
    {
        if (const auto deleted = song::deleteTransposeRendersOfRemovedBackingTracks (packageRoot, song); deleted > 0)
            core::logEvent (core::Level::info, "ui.transpose_renders_deleted", { { "count", deleted } });
        sweptBackingTracks = std::move (backingTrackIds);
    }

    auto missing = song::missingTransposeRenders (packageRoot, song);
    std::erase_if (missing, [this] (const song::TransposeRenderJob& job) { return failedTranspose.sources.contains (job.backingTrackId); });
    if (! missing.empty())
    {
        transposeRun = song::TransposeRun::start (song.sampleRate, target, std::move (missing), now);
        return failure;
    }
    if (heardShift != target && failedTranspose.sources.empty())
    {
        core::logEvent (core::Level::info, "ui.transpose_heard", { { "from_semitones", heardShift.semitones }, { "from_cents", heardShift.cents }, { "to_semitones", target.semitones }, { "to_cents", target.cents } });
        heardShift = target;
        song::deleteTransposeRendersExcept (packageRoot, target);
        playHeard();
    }
    return failure;
}

song::PracticeRenderKey ListeningRenders::practiceKey (const song::Song& song, const ListeningWanted& wanted)
{
    if (wanted.practiceSpeedPercent == song::normalPracticeSpeed)
        return {};
    return { .speedPercent = wanted.practiceSpeedPercent, .transpose = song.transpose };
}

std::optional<song::SampleRange> ListeningRenders::practiceLoopWindow (const song::Song& song, const ListeningWanted& wanted)
{
    if (! wanted.heardLoop.has_value() || wanted.practiceSpeedPercent == song::normalPracticeSpeed)
        return std::nullopt;
    return song::loopRenderWindow (wanted.heardLoop->start, wanted.heardLoop->end, song.sampleRate);
}

bool ListeningRenders::practiceRunIsWanted (const song::Song& song, const ListeningWanted& wanted) const
{
    return practiceRun->key() == practiceKey (song, wanted) && practiceRun->loopWindow() == practiceLoopWindow (song, wanted);
}

void ListeningRenders::deleteUnheardPracticeRenders (const song::Song& song, const ListeningWanted& wanted)
{
    if (const auto deleted = song::deletePracticeRendersExcept (practiceRoot, song, heardPractice, practiceLoopWindow (song, wanted)); deleted > 0)
        core::logEvent (core::Level::info, "ui.practice_renders_deleted", { { "count", deleted } });
}

std::optional<core::Error> ListeningRenders::advancePracticeSpeed (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now)
{
    const auto target = practiceKey (song, wanted);
    const auto window = practiceLoopWindow (song, wanted);
    failedPractice.forgetUnless (target);
    std::optional<core::Error> failure;
    if (practiceRun != nullptr)
    {
        if (! practiceRunIsWanted (song, wanted))
        {
            practiceRun->cancel (now);
            practiceRun.reset();
        }
        else if (auto ended = practiceRun->advance (now))
        {
            const auto failedSource = practiceRun->failedSource();
            practiceRun.reset();
            if (! ended->has_value())
            {
                failure = ended->error();
                failedPractice.add (target, *failedSource);
            }
            else if (heardPractice == target)
            {
                playHeard(); // sources that had no render, or only a Loop's, play theirs now
                deleteUnheardPracticeRenders (song, wanted);
            }
        }
        else
            return std::nullopt;
    }

    const auto missingOf = [&] (std::optional<song::SampleRange> rendersWindow) {
        auto missing = song::missingPracticeRenders (packageRoot, practiceRoot, song, target, rendersWindow);
        std::erase_if (missing, [this] (const song::PracticeRenderJob& job) { return failedPractice.sources.contains (job.source); });
        return missing;
    };
    const auto startRun = [&] (std::vector<song::PracticeRenderJob> jobs) {
        // A folder that cannot be made fails the render's write, handed back by the run.
        practiceRoot.createDirectory();
        practiceRun = song::PracticeSpeedRun::start (song.sampleRate, target, window, std::move (jobs), now);
    };
    // While a Loop loops, its range renders first (issue #231); the whole Song's after.
    if (window.has_value())
        if (auto missingInLoop = missingOf (window); ! missingInLoop.empty())
        {
            startRun (std::move (missingInLoop));
            return failure;
        }
    auto missing = missingOf (std::nullopt);
    // While a Loop loops it is heard once its renders are there; otherwise once every render is.
    if (heardPractice != target && failedPractice.sources.empty() && (missing.empty() || window.has_value()))
    {
        core::logEvent (core::Level::info,
                        "ui.practice_speed_heard",
                        { { "from_percent", heardPractice.speedPercent }, { "to_percent", target.speedPercent }, { "semitones", target.transpose.semitones }, { "cents", target.transpose.cents } });
        heardPractice = target;
        playHeard();
        deleteUnheardPracticeRenders (song, wanted);
    }
    if (! missing.empty())
        startRun (std::move (missing));
    return failure;
}

song::PracticeRenderKey ListeningRenders::octaveKey (const song::Song& song) const
{
    // At 100% the Backing Tracks play at the heard Transpose; at another speed, at the renders'.
    const auto transpose = heardPractice.speedPercent == song::normalPracticeSpeed ? heardShift : heardPractice.transpose;
    return { .speedPercent = heardPractice.speedPercent, .transpose = transpose, .octaveShift = song::focusOctaveShift (song) };
}

bool ListeningRenders::octaveRunIsWanted (const song::Song& song, const ListeningWanted& wanted) const
{
    return wanted.octaveShiftedTrack == octaveRunTrackId && octaveRun->key() == octaveKey (song);
}

std::optional<engine::OctaveShiftedTrack> ListeningRenders::heardOctaveShift (const song::Song& song, const ListeningWanted& wanted) const
{
    if (! wanted.octaveShiftedTrack.has_value())
        return std::nullopt;
    const auto& trackId = *wanted.octaveShiftedTrack;
    return engine::OctaveShiftedTrack { .trackId = trackId, .renders = song::practiceRendersOf (octaveFolder(), octaveShiftedSong (song, trackId), octaveKey (song)) };
}

std::optional<core::Error> ListeningRenders::advanceOctaveShift (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now)
{
    const auto& trackId = wanted.octaveShiftedTrack;
    const auto key = trackId.has_value() ? octaveKey (song) : song::PracticeRenderKey {};
    failedOctave.forgetUnless ({ trackId, key });
    std::optional<core::Error> failure;
    if (octaveRun != nullptr)
    {
        if (! octaveRunIsWanted (song, wanted))
        {
            octaveRun->cancel (now);
            octaveRun.reset();
        }
        else if (auto ended = octaveRun->advance (now))
        {
            const auto failedSource = octaveRun->failedSource();
            octaveRun.reset();
            if (! ended->has_value())
            {
                failure = ended->error();
                failedOctave.add ({ trackId, key }, *failedSource);
                playHeard(); // the renders made before the failed one play
            }
            else
            {
                playHeard(); // the renders made play now
                if (const auto deleted = song::deletePracticeRendersExcept (octaveFolder(), octaveShiftedSong (song, *trackId), key); deleted > 0)
                    core::logEvent (core::Level::info, "ui.octave_renders_deleted", { { "count", deleted } });
            }
        }
        else
            return std::nullopt;
    }
    if (! trackId.has_value())
        return failure;

    auto missing = song::missingPracticeRenders (packageRoot, octaveFolder(), octaveShiftedSong (song, *trackId), key);
    std::erase_if (missing, [this] (const song::PracticeRenderJob& job) { return failedOctave.sources.contains (job.source); });
    if (! missing.empty())
    {
        // A folder that cannot be made fails the render's write, handed back by the run.
        octaveFolder().createDirectory();
        octaveRun = song::PracticeSpeedRun::start (song.sampleRate, key, std::nullopt, std::move (missing), now);
        octaveRunTrackId = *trackId;
    }
    return failure;
}
} // namespace overdub::ui
