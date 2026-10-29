#pragma once

#include "core/Error.h"
#include "song/RenderWorker.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// Practice Speed (ADR 0009, issue #212): the whole Song played slower or faster at its own pitch,
// for rehearsing. It is not part of the Song: not saved, not an undo step (ui::OpenSong holds it).
//
// At a Practice Speed other than 100, each source some Clip plays is heard through a render made in
// one pass from its derived audio (song/TransposeRender.h renderAtPracticeSpeed), with the Song's
// Transpose applied to a Backing Track. Renders are temporary files in a folder outside the package:
//   take-<id>.speed80.wav                  a Take's (every Transpose)
//   backing-<id>.transpose+3.speed80.wav   a Backing Track's at Transpose +3 (transposeFileTag)
// While a Loop loops in Transcribe mode (issue #231, ADR 0009 "The active Loop renders first"), a
// source is first rendered only over the source samples its Clips play in the Loop's window (the
// Loop plus loopRenderMarginSeconds each side), as a render of the whole length that is silent
// outside them, named by that source range:
//   take-<id>.speed80.range12000-96000.wav
// A source's whole render, once there, is preferred to any range render.
namespace overdub::song
{
inline constexpr int normalPracticeSpeed = 100; // the Song's own speed: nothing is rendered
inline constexpr int minPracticeSpeed = 25;
inline constexpr int maxPracticeSpeed = 125;
inline constexpr int halfPracticeSpeed = 50;
inline constexpr int practiceSpeedStep = 5; // clamped to min..max

// The Practice Speed, when it is a whole percent from 25 to 125.
// Errors: song.practice_speed_out_of_range (speed_percent, min, max).
core::Expected<int> checkPracticeSpeed (int speedPercent);

inline constexpr double loopRenderMarginSeconds = 1.0;

// Samples [start, end): of the Song's timeline, or of a source's file.
struct SampleRange
{
    SampleCount start = 0;
    SampleCount end = 0;

    bool operator== (const SampleRange&) const = default;
};

// The Song samples a Loop's renders cover: [loopStart, loopEnd) widened by loopRenderMarginSeconds
// on each side, from 0.
SampleRange loopRenderWindow (SampleCount loopStart, SampleCount loopEnd, int sampleRate);

// The source samples the Clips of `source` play in Song samples `window`: from the first to the
// last, across every Track. None when no Clip of it plays there.
std::optional<SampleRange> sourceRangeIn (const Song& song, const ClipSource& source, SampleRange window);

// What a set of renders is made for: a Practice Speed, and the Transpose its Backing Tracks are at.
// The default (100%) needs no render.
struct PracticeRenderKey
{
    int speedPercent = normalPracticeSpeed;
    Transpose transpose;

    bool operator== (const PracticeRenderKey&) const = default;
};

// A source's render at `key` in `folder` (see the header comment).
juce::File practiceRenderFileFor (const juce::File& folder, const ClipSource& source, PracticeRenderKey key);
// A source's render at `key` over only `sourceRange` of its samples (see the header comment).
juce::File practiceRangeRenderFileFor (const juce::File& folder, const ClipSource& source, PracticeRenderKey key, SampleRange sourceRange);

// One render to make: a source's derived audio into its render.
struct PracticeRenderJob
{
    ClipSource source;
    juce::File from;   // the Take's file, or the Backing Track's derived WAV
    juce::File to;     // practiceRenderFileFor
    Transpose transpose; // the key's for a Backing Track, none ({}) for a Take
    // Set for a Loop's render: only these source samples are rendered (renderAtPracticeSpeed).
    std::optional<SampleRange> sourceRange = std::nullopt;
};

// The renders `song` lacks at `key`, in Track and Clip order: one per distinct source some Clip
// plays whose file is there and whose whole render is not. With a Loop's `window`
// (loopRenderWindow), only those whose Clips play in it, each over sourceRangeIn (window), and only
// when that range render is not there either. None at 100%.
std::vector<PracticeRenderJob> missingPracticeRenders (const juce::File& packageRoot,
                                                       const juce::File& folder,
                                                       const Song& song,
                                                       PracticeRenderKey key,
                                                       std::optional<SampleRange> window = std::nullopt);

// The renders at `key` that are there, of the sources some Clip of `song` plays: each source's
// whole render, or else, with a Loop's `window`, its render over sourceRangeIn (window).
std::map<ClipSource, juce::File> practiceRendersOf (const juce::File& folder, const Song& song, PracticeRenderKey key, std::optional<SampleRange> window = std::nullopt);

// Deletes every file in `folder` but practiceRendersOf (folder, song, key, window). Returns how
// many.
int deletePracticeRendersExcept (const juce::File& folder, const Song& song, PracticeRenderKey key, std::optional<SampleRange> window = std::nullopt);

// The renders of one key, made one after another by renderAtPracticeSpeed on a worker thread;
// advance() (message thread) hands back how it ended. It reads and writes only its jobs' files, so
// the Song may change while it runs. `loopWindow` is the Loop's window (loopRenderWindow) it was
// started for, none outside a Loop: the owner cancels a run whose key or window is no longer
// wanted. Its scope is "loop" when its jobs are range renders, else "song". Logs
// song.practice_speed_started (info: speed_percent, semitones, cents, renders, scope, and for a "loop"
// run window_start and window_end in Song samples) and, once every render is written, song.practice_speed_rendered (info: the
// same and duration_ms). The first failed render ends the run (its Error with source_kind and
// source_id). cancel() stops the worker after its current block, logging
// song.practice_speed_cancelled (info: the same, renders_done, duration_ms) unless every render
// was written. Destroying a running run cancels it.
class PracticeSpeedRun
{
public:
    using Clock = std::chrono::steady_clock;

    static std::unique_ptr<PracticeSpeedRun> start (int sampleRate, PracticeRenderKey key, std::optional<SampleRange> loopWindow, std::vector<PracticeRenderJob> jobs, Clock::time_point now);

    ~PracticeSpeedRun();

    PracticeSpeedRun (const PracticeSpeedRun&) = delete;
    PracticeSpeedRun& operator= (const PracticeSpeedRun&) = delete;

    PracticeRenderKey key() const { return target; }
    std::optional<SampleRange> loopWindow() const { return window; }

    // Once the worker is done: none on success, or the failed render's Error. Nothing while it
    // runs. The run is over once it returns.
    std::optional<core::Expected<void>> advance (Clock::time_point now);
    // The source whose render failed, once advance() has handed back a failure; none otherwise.
    std::optional<ClipSource> failedSource() const;

    // Stops the worker and waits for it. Nothing once the run is over.
    void cancel (Clock::time_point now);

private:
    PracticeSpeedRun (PracticeRenderKey key, std::optional<SampleRange> loopWindow, std::vector<PracticeRenderJob> jobs, Clock::time_point startedAt);

    core::Fields fields (Clock::time_point now) const;

    PracticeRenderKey target;
    std::optional<SampleRange> window;
    std::vector<PracticeRenderJob> jobs;
    Clock::time_point startedAt;
    bool failed = false;
    std::unique_ptr<RenderWorker> worker;
};
} // namespace overdub::song
