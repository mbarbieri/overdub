#pragma once

#include "core/Error.h"
#include "engine/Engine.h"
#include "engine/RenderPlan.h"
#include "song/PracticeSpeed.h"
#include "song/Song.h"
#include "ui/ListeningRenders.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <vector>

// Listening (issue #249, ADR 0009, ADR 0010): what playback hears of the open Song, besides the
// Song itself. It holds whether the window is transcribing and the Practice Speed (neither is in
// the Song), the rule that ties the Practice Speed to the heard Loop (issue #230), and the
// ListeningRenders (ui/ListeningRenders.h) that follow them. heard() answers everything the engine
// is given for the Song in one Heard; the owner compares the Heard before and after a change to
// decide how the engine follows it.
//
// - While transcribing, the heard Song has the Transcribe Mix's mute and solo
//   (song::withTranscribeMix), the heard Loop is the active Loop while looping, and the Focus
//   Track's band filter (when on) and octave shift (when not 0) are heard. Otherwise none of these.
// - The Practice Speed is 100% at first. Setting it retries its failed renders. While a Loop is
//   heard, setPracticeSpeed hands back the Song with that speed stored as the Loop's, for the owner
//   to store outside history.
// - hearLoopPracticeSpeed() plays at the heard Loop's own speed. Leaving Transcribe mode returns
//   to 100% (ADR 0010); entering it plays at the looping Loop's speed. Each only when the owner
//   says the Practice Speed may change now.
//
// No setter follows the renders: the owner calls follow() once, after its whole change, so a run
// is started only for what is wanted in the end. Logs ui.practice_speed_set (info: from_percent,
// to_percent) and ui.transcribe_mix_heard (debug: heard). Message thread only.
namespace overdub::ui
{
// Everything the engine is given for the open Song.
struct Heard
{
    // The Song as the engine reads it: at the heard Transpose, with the Transcribe Mix's mute and
    // solo while transcribing, and without the Transcribe settings (the Focus Track, the Transcribe
    // Mix, each Track's octave shift and band filter, the active Loop and looping), which reach the
    // engine only through the fields below.
    song::Song song;
    std::optional<engine::LoopRange> loop;
    std::optional<engine::HeardBandFilter> bandFilter;
    std::optional<song::Id> octaveShiftedTrack;
    // The octave-shifted Track's shift; 0 when none is.
    int octaveShift = 0;
};

class Listening
{
public:
    using Clock = ListeningRenders::Clock;

    // `playHeard` is ListeningRenders': called whenever the renders playback uses change.
    Listening (juce::File packageRoot, const song::Song& song, std::function<void()> playHeard);

    Heard heard (const song::Song& song) const;
    // `song` with the Transcribe Mix's mute and solo while transcribing; as it is otherwise.
    song::Song withHeardMix (song::Song song) const;
    // What the engine plays at the heard Practice Speed (ListeningRenders::practicePlayback).
    engine::PracticePlayback practicePlayback (const song::Song& song) const;

    // ListeningRenders::advance and follow with what is wanted now.
    std::vector<core::Error> advance (const song::Song& song, Clock::time_point now);
    void follow (const song::Song& song);

    // Whether the window shows Transcribe mode (see the header comment).
    void setTranscribing (const song::Song& song, bool on, bool practiceSpeedMayChange);
    bool isTranscribing() const { return transcribing; }

    // `speedPercent` is in range (song::checkPracticeSpeed). Returns the Song with it stored as the
    // heard Loop's, when a Loop is heard at another speed; none otherwise.
    std::optional<song::Song> setPracticeSpeed (const song::Song& song, int speedPercent);
    // Plays at the heard Loop's Practice Speed; nothing while no Loop is heard.
    void hearLoopPracticeSpeed (const song::Song& song, bool practiceSpeedMayChange);
    int practiceSpeed() const { return practiceTarget; }

    // Playback is not at the Song's Transpose yet, or renders for it are being made.
    bool isTransposing (const song::Song& song) const;
    const ListeningRenders& renders() const { return listeningRenders; }

private:
    std::optional<engine::LoopRange> heardLoop (const song::Song& song) const;
    // The active Loop while it is heard; null otherwise.
    const song::Loop* heardActiveLoop (const song::Song& song) const;
    std::optional<song::Id> octaveShiftedTrack (const song::Song& song) const;
    ListeningWanted wanted (const song::Song& song) const;
    // Sets the target (ui.practice_speed_set when it changes) and forgets failed renders.
    void setPracticeTarget (int speedPercent);

    bool transcribing = false;
    int practiceTarget = song::normalPracticeSpeed;
    ListeningRenders listeningRenders;
};
} // namespace overdub::ui
