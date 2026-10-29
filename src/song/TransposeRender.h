#pragma once

#include "core/Error.h"
#include "song/PracticeSpeed.h"
#include "song/RenderWorker.h"
#include "song/Transpose.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

// The Transpose render (ADR 0009, issues #209, #232): a Backing Track's derived WAV pitch-shifted by
// whole semitones and cents together with Signalsmith Stretch, its length unchanged, in one pass
// from the derived WAV (never from an earlier render). Offline only: it allocates and takes
// seconds, so it runs on a worker thread, never on the audio thread. Deterministic: the stretcher is seeded.
namespace overdub::song
{
// Reads `source` (a 32-bit float WAV at `sampleRate`, mono or stereo), shifts it by `transpose` and
// writes it to `destination` as a float WAV at `sampleRate` (temp + rename). A stop request is
// heard between blocks. On any Error nothing is left at `destination` or its temp file.
// Errors: song.transpose_render_failed (path, semitones, cents, step read|write, cause: the read or
// write Error's code), song.transpose_render_cancelled (path, semitones, cents).
core::Expected<void> renderTransposed (const juce::File& source, const juce::File& destination, int sampleRate, Transpose transpose, std::stop_token stop);

// The Practice Speed render (ADR 0009, issue #212): `source` played at `speedPercent` of its speed
// at its own pitch, shifted by `transpose` (a Backing Track's Transpose; 0 for a Take), in one pass.
// Written to `destination` as a float WAV at `sampleRate` (temp + rename), llround (length × 100 /
// speedPercent) samples long: source sample s sounds at about s × 100 / speedPercent. As
// renderTransposed otherwise. With a `sourceRange` (a Loop's render, issue #231), only those source
// samples (clamped to the source) are stretched, from their own first sample, and written where
// the whole render has them, from llround (start × 100 / speedPercent); the rest of the same length
// is silent. Errors: song.practice_render_failed (path, semitones, cents, speed_percent, step
// read|write, cause), song.practice_render_cancelled (path, semitones, cents, speed_percent).
core::Expected<void> renderAtPracticeSpeed (const juce::File& source,
                                            const juce::File& destination,
                                            int sampleRate,
                                            Transpose transpose,
                                            int speedPercent,
                                            std::stop_token stop,
                                            std::optional<SampleRange> sourceRange = std::nullopt);

// The renders of one Transpose (missingTransposeRenders' jobs), made one after another by
// renderTransposed on a worker thread; advance() (message thread) hands back how it ended. It
// reads and writes only the jobs' files, so the Song may change while it runs: the owner decides
// what the renders are still good for. Logs song.transpose_started (info: semitones, cents, renders)
// and, when every render is written, song.transpose_rendered (info: semitones, cents, renders,
// duration_ms).
// The first failed render ends the run; the renders written before it stay. cancel() stops the
// worker after its current block and, unless every render was written already, logs
// song.transpose_cancelled (info: semitones, cents, renders_done, renders, duration_ms); how the run ended
// is dropped. Destroying a running run cancels it.
class TransposeRun
{
public:
    using Clock = std::chrono::steady_clock;

    static std::unique_ptr<TransposeRun> start (int sampleRate, Transpose shift, std::vector<TransposeRenderJob> jobs, Clock::time_point now);

    ~TransposeRun();

    TransposeRun (const TransposeRun&) = delete;
    TransposeRun& operator= (const TransposeRun&) = delete;

    Transpose transpose() const { return target; }
    const std::vector<TransposeRenderJob>& renderJobs() const { return jobs; }

    // Once the worker is done: none on success, or the failed render's Error
    // (song.transpose_render_failed, with its backing_track_id). Nothing while it runs. The run is
    // over once it returns.
    std::optional<core::Expected<void>> advance (Clock::time_point now);

    // Stops the worker and waits for it. Nothing once the run is over.
    void cancel (Clock::time_point now);

private:
    TransposeRun (Transpose shift, std::vector<TransposeRenderJob> jobs, Clock::time_point startedAt);

    core::Fields fields() const;

    Transpose target;
    std::vector<TransposeRenderJob> jobs;
    Clock::time_point startedAt;
    std::unique_ptr<RenderWorker> worker;
};
} // namespace overdub::song
