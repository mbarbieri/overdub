#include "ui/Listening.h"

#include "core/Log.h"
#include "song/LoopEdits.h"
#include "song/TranscribeMix.h"

#include <utility>

namespace overdub::ui
{
namespace
{
// The Transcribe settings reach the engine as Heard's own fields, never through the Song: cleared,
// a change of them alone is no change of the heard Song.
song::Song withoutTranscribeSettings (song::Song song)
{
    song.focusTrackId.reset();
    song.activeLoopId.reset();
    song.looping = false;
    for (auto& track : song.tracks)
    {
        track.transcribeMute = false;
        track.transcribeSolo = false;
        track.octaveShift = 0;
        track.bandFilter = {};
    }
    return song;
}
} // namespace

Listening::Listening (juce::File packageRoot, const song::Song& song, std::function<void()> playHeard)
    : listeningRenders (std::move (packageRoot), song, std::move (playHeard))
{
}

Heard Listening::heard (const song::Song& song) const
{
    Heard heard { .song = withoutTranscribeSettings (withHeardMix (song)), .loop = heardLoop (song), .octaveShiftedTrack = octaveShiftedTrack (song) };
    heard.song.transpose = listeningRenders.heardTranspose();
    const auto* focus = song::focusTrack (song);
    if (heard.octaveShiftedTrack.has_value())
        heard.octaveShift = focus->octaveShift;
    if (transcribing && focus != nullptr && focus->bandFilter.on)
        heard.bandFilter = engine::HeardBandFilter { .trackId = focus->id, .lowHz = focus->bandFilter.lowHz, .highHz = focus->bandFilter.highHz };
    return heard;
}

song::Song Listening::withHeardMix (song::Song song) const
{
    return transcribing ? song::withTranscribeMix (std::move (song)) : song;
}

engine::PracticePlayback Listening::practicePlayback (const song::Song& song) const
{
    return listeningRenders.practicePlayback (song, wanted (song));
}

std::vector<core::Error> Listening::advance (const song::Song& song, Clock::time_point now)
{
    return listeningRenders.advance (song, wanted (song), now);
}

void Listening::follow (const song::Song& song)
{
    listeningRenders.follow (song, wanted (song));
}

void Listening::setTranscribing (const song::Song& song, bool on, bool practiceSpeedMayChange)
{
    if (on == transcribing)
        return;
    transcribing = on;
    core::logEvent (core::Level::debug, "ui.transcribe_mix_heard", { { "heard", on } });
    if (! practiceSpeedMayChange)
        return;
    // Record and Edit play at 100%; Transcribe brings back the looping Loop's speed (ADR 0010).
    if (! on && practiceTarget != song::normalPracticeSpeed)
        changePracticeTarget (song, song::normalPracticeSpeed);
    hearLoopPracticeSpeed (song);
}

std::optional<song::Song> Listening::setPracticeSpeed (const song::Song& song, int speedPercent)
{
    // The same value again tries the renders that failed again.
    changePracticeTarget (song, speedPercent);
    const auto* loop = heardLoop (song).has_value() ? song::activeLoop (song) : nullptr;
    if (loop == nullptr || loop->practiceSpeedPercent == practiceTarget)
        return std::nullopt;
    auto changed = song::setLoopPracticeSpeed (song, loop->id, practiceTarget);
    OVERDUB_CHECK (changed.has_value(), "ui.loop_practice_speed_unset");
    core::logEvent (core::Level::info, "ui.loop_practice_speed_set", { { "loop_id", loop->id }, { "speed_percent", practiceTarget } });
    return std::move (*changed);
}

void Listening::hearLoopPracticeSpeed (const song::Song& song)
{
    const auto* loop = heardLoop (song).has_value() ? song::activeLoop (song) : nullptr;
    if (loop != nullptr && loop->practiceSpeedPercent != practiceTarget)
        changePracticeTarget (song, loop->practiceSpeedPercent);
}

bool Listening::isTransposing (const song::Song& song) const
{
    return listeningRenders.isTransposeRendering() || listeningRenders.heardTranspose() != song.transpose;
}

std::optional<engine::LoopRange> Listening::heardLoop (const song::Song& song) const
{
    const auto* loop = song::activeLoop (song);
    if (! transcribing || ! song.looping || loop == nullptr)
        return std::nullopt;
    return engine::LoopRange { loop->start, loop->end };
}

std::optional<song::Id> Listening::octaveShiftedTrack (const song::Song& song) const
{
    const auto* track = song::focusTrack (song);
    if (! transcribing || track == nullptr || track->octaveShift == 0)
        return std::nullopt;
    return track->id;
}

ListeningWanted Listening::wanted (const song::Song& song) const
{
    return { .practiceSpeedPercent = practiceTarget, .heardLoop = heardLoop (song), .octaveShiftedTrack = octaveShiftedTrack (song) };
}

void Listening::changePracticeTarget (const song::Song& song, int speedPercent)
{
    if (speedPercent != practiceTarget)
        core::logEvent (core::Level::info, "ui.practice_speed_set", { { "from_percent", practiceTarget }, { "to_percent", speedPercent } });
    practiceTarget = speedPercent;
    listeningRenders.retryPracticeRenders();
    follow (song);
}
} // namespace overdub::ui
