#pragma once

#include "core/Error.h"
#include "engine/Engine.h"
#include "engine/RenderPlan.h"
#include "song/PracticeSpeed.h"
#include "song/Song.h"
#include "song/SongPackage.h"
#include "song/TransposeRender.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// The Listening Renders (issue #247, ADR 0009, ADR 0010): the offline renders playback plays
// instead of the Song's own audio, and when it switches to them. Three kinds follow what the open
// Song wants:
//
// - Transpose (issue #209): every Backing Track's render of the Song's Transpose, in the package.
//   heardTranspose() follows the Song's Transpose only once every render is there; until then the
//   previous value plays. Switching deletes every other value's renders. At Transpose 0 the
//   derived audio plays at once. A render of a Backing Track the Song no longer has (an undone
//   import or Separation) is deleted once no render runs (ui.transpose_renders_deleted: count); a
//   run rendering one is cancelled at once and started again for the renders still missing.
// - Practice Speed (issue #212): at any speed but 100%, every source some Clip plays, rendered into
//   practiceFolder() (a temp folder outside the package, deleted with this) with the Song's
//   Transpose on its Backing Tracks. heardPracticeSpeed() follows the wanted speed once every
//   render is there (ui.practice_speed_heard), and every other render is deleted
//   (ui.practice_renders_deleted: count). While a Loop is heard (issue #231) its window
//   (song::loopRenderWindow) renders first and is heard as soon as it is there; a second run then
//   renders the whole Song. At the heard value, a source with no render yet plays silence until
//   the run that makes it ends.
// - Octave shift (issue #233): the octave-shifted Track's sources, rendered into the "octave"
//   folder in practiceFolder() at the heard Practice Speed and Transpose plus the octaves. It is
//   heard at once: a source with no render yet plays silence until its run ends.
//
// A run is cancelled as soon as what it renders is no longer wanted. A failed render keeps what
// plays, is handed back by advance() once (song.transpose_render_failed or
// song.practice_render_failed) and is not tried again until its key changes (the Transpose; the
// Practice Speed or Transpose; the Track, its shift or the heard key) or retryPracticeRenders().
//
// Whenever what heardTranspose(), heardPracticeSpeed() or practicePlayback() answers changes, it
// calls `playHeard` (the owner reloads the engine), before it deletes renders no longer heard.
//
// Message thread only.
namespace overdub::ui
{
// What the open Song wants heard, besides the Song itself.
struct ListeningWanted
{
    int practiceSpeedPercent = song::normalPracticeSpeed;
    // The Loop playback loops over (OpenSong::heardLoop): its window renders first.
    std::optional<engine::LoopRange> heardLoop;
    // The Track whose octave shift is heard (OpenSong::octaveShiftedTrack).
    std::optional<song::Id> octaveShiftedTrack;
};

class ListeningRenders
{
public:
    using Clock = song::SongPackage::Clock;

    // Deletes the Transpose renders of other values than the Song's. The derived audio plays until
    // every render of the Song's Transpose is there; follow() starts the missing ones.
    ListeningRenders (juce::File packageRoot, const song::Song& song, std::function<void()> playHeard);
    // Cancels the runs and deletes practiceFolder().
    ~ListeningRenders();

    ListeningRenders (const ListeningRenders&) = delete;
    ListeningRenders& operator= (const ListeningRenders&) = delete;

    // Call regularly (window timer): ends the runs that are done, switches what is heard, and
    // starts the renders still missing. Returns the failed renders' Errors, not logged.
    std::vector<core::Error> advance (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now);
    // After the Song or `wanted` changed: cancels the runs no longer wanted and starts the renders
    // now missing, without waiting for a run that is still wanted.
    void follow (const song::Song& song, const ListeningWanted& wanted);
    // Forgets the Practice Speed renders that failed, so follow() tries them again.
    void retryPracticeRenders() { failedPractice.sources.clear(); }

    // The Transpose playback uses.
    song::Transpose heardTranspose() const { return heardShift; }
    // The Practice Speed playback uses.
    int heardPracticeSpeed() const { return heardPractice.speedPercent; }
    bool isTransposeRendering() const { return transposeRun != nullptr; }
    bool isPracticeRendering() const { return practiceRun != nullptr; }
    bool isOctaveRendering() const { return octaveRun != nullptr; }
    // What the engine plays at heardPracticeSpeed(): the renders that are there of the heard key
    // (with the heard Loop's range renders, for sources not rendered whole yet) and the
    // octave-shifted Track's.
    engine::PracticePlayback practicePlayback (const song::Song& song, const ListeningWanted& wanted) const;
    // Where the Practice Speed and octave-shift renders are.
    const juce::File& practiceFolder() const { return practiceRoot; }

private:
    // The renders that failed for one key: not tried again while that key is wanted.
    template <typename Key, typename Source>
    struct FailedRenders
    {
        Key key {};
        std::set<Source> sources;

        void forgetUnless (const Key& wanted)
        {
            if (wanted != key)
                sources.clear();
        }
        void add (Key failedKey, Source source)
        {
            key = std::move (failedKey);
            sources.insert (std::move (source));
        }
    };

    std::optional<core::Error> advanceTranspose (const song::Song& song, Clock::time_point now);
    std::optional<core::Error> advancePracticeSpeed (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now);
    std::optional<core::Error> advanceOctaveShift (const song::Song& song, const ListeningWanted& wanted, Clock::time_point now);
    // The key the Practice Speed renders are wanted for ({} at 100%).
    static song::PracticeRenderKey practiceKey (const song::Song& song, const ListeningWanted& wanted);
    // The heard Loop's song::loopRenderWindow while the Practice Speed is not 100%; none otherwise.
    static std::optional<song::SampleRange> practiceLoopWindow (const song::Song& song, const ListeningWanted& wanted);
    bool practiceRunIsWanted (const song::Song& song, const ListeningWanted& wanted) const;
    void deleteUnheardPracticeRenders (const song::Song& song, const ListeningWanted& wanted);
    // The key the octave-shifted Track's renders are made for: the heard Practice Speed and
    // Transpose, and its octave shift.
    song::PracticeRenderKey octaveKey (const song::Song& song) const;
    bool octaveRunIsWanted (const song::Song& song, const ListeningWanted& wanted) const;
    std::optional<engine::OctaveShiftedTrack> heardOctaveShift (const song::Song& song, const ListeningWanted& wanted) const;
    juce::File octaveFolder() const { return practiceRoot.getChildFile ("octave"); }

    juce::File packageRoot;
    std::function<void()> playHeard;

    song::Transpose heardShift;
    FailedRenders<song::Transpose, song::Id> failedTranspose;
    // The Song's Backing Tracks when the renders of removed ones were last deleted; none after a
    // render ended or was cancelled, which deletes them again.
    std::optional<std::vector<song::Id>> sweptBackingTracks;
    std::unique_ptr<song::TransposeRun> transposeRun;

    song::PracticeRenderKey heardPractice;
    FailedRenders<song::PracticeRenderKey, song::ClipSource> failedPractice;
    FailedRenders<std::pair<std::optional<song::Id>, song::PracticeRenderKey>, song::ClipSource> failedOctave;
    juce::File practiceRoot;
    // After practiceRoot: each run writes there, and is cancelled before the folder is deleted.
    std::unique_ptr<song::PracticeSpeedRun> practiceRun;
    std::unique_ptr<song::PracticeSpeedRun> octaveRun;
    song::Id octaveRunTrackId;
};
} // namespace overdub::ui
