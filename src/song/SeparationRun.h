#pragma once

#include "core/Error.h"
#include "song/SeparationHelper.h"
#include "song/SongPackage.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// One Separation of a Track's Backing Track (issue #114, ADR 0007), driven from a timer like an
// export: start() checks the Track and the model and starts the helper on the Backing Track's
// original file (not its derived WAV, so the audio is not resampled twice), writing its parts into
// a fresh output folder under the tools' work folder; advance() reads its progress lines and, once
// it has exited with success, adds the six parts to the Song as one undo step
// (SongPackage::addSeparation). Logs song.separation_started (info: track_id,
// source_backing_track_id, model, input, output) and song.separation_finished (info: track_id,
// source_backing_track_id, backing_track_ids, model, duration_ms).
//
// A helper that fails (any non-zero exit, a signal, or a part missing after a success) is the Error
// song.separation_failed (exit_code, signal, helper_error, helper_message, helper_error_line: its
// raw stderr; or, for a part missing after a success, reason "part_missing" and path), or
// song.separation_model_invalid with the same fields when the helper refused the model
// (separation.model_missing, model_unloadable, model_mismatch). Writing the parts into the package
// after a success can fail too (SongPackage::addSeparation's Errors, which leave no file and no
// undo step). In every case the Song is unchanged. A stdout line that is not a JSON event is
// skipped (song.separation_line_ignored, warn: track_id, line). cancel() kills the helper at once
// (SIGKILL, not waiting for a chunk) and logs song.separation_cancelled (info: track_id,
// source_backing_track_id, chunks_done, chunks, duration_ms). Every end, cancel and destroying the
// run (which kills a helper that still runs) deletes the output folder.
//
// Message thread only. The package must outlive the run.
namespace overdub::song
{
// What a Separation needs from the machine: the installed model, how to start the helper, and
// where output folders go (default: the system temp folder).
struct SeparationTools
{
    juce::File model;
    StartSeparationHelper startHelper;
    juce::File workFolder = juce::File::getSpecialLocation (juce::File::tempDirectory);
};

// The helper's progress so far, from its separation.started and separation.chunk_done lines.
struct SeparationProgress
{
    int chunksDone = 0;
    int chunks = 0; // 0 until the helper said how many

    // 0..1: the chunks done out of all.
    double fraction() const { return chunks > 0 ? (double) chunksDone / (double) chunks : 0.0; }
};

class SeparationRun
{
public:
    using Clock = SongPackage::Clock;

    // `now` starts the duration logged when it finishes.
    // Errors: separableBackingTrack's (song.track_not_found, song.track_not_separable),
    // song.separation_unavailable (no helper to start), checkSeparationModel's
    // (song.separation_model_missing, song.separation_model_invalid), the helper's start Error
    // (song.separation_start_failed).
    static core::Expected<std::unique_ptr<SeparationRun>> start (SongPackage& package, const Id& trackId, const SeparationTools& tools, Clock::time_point now);

    ~SeparationRun();

    SeparationRun (const SeparationRun&) = delete;
    SeparationRun& operator= (const SeparationRun&) = delete;

    const SeparationProgress& progress() const { return shownProgress; }
    // The source Backing Track's original file name ("Band Mix.mp3"), for the dialog.
    const std::string& sourceName() const { return sourceFileName; }
    // The folder the helper writes its parts into.
    const juce::File& outputFolder() const { return output; }

    // Reads the helper's lines; once it has ended, returns the new Backing Track ids (the
    // Separation is in the Song) or the Error. None while it runs. The run is over once it returns.
    std::optional<core::Expected<std::vector<Id>>> advance (Clock::time_point now);

    // Kills a helper that still runs, deletes its output folder and logs song.separation_cancelled;
    // the Song is unchanged. Nothing once the run is over.
    void cancel (Clock::time_point now);

private:
    SeparationRun (SongPackage& package, Id trackId, Id sourceBackingTrackId, std::string sourceName, std::string model, juce::File output, Clock::time_point startedAt);

    void readProgress (const std::vector<std::string>& lines);
    core::Expected<std::vector<Id>> finish (const HelperExit& exit, Clock::time_point now);

    SongPackage& package;
    Id trackId;
    Id sourceBackingTrackId;
    std::string sourceFileName;
    std::string model;
    juce::File output;
    Clock::time_point startedAt;
    SeparationProgress shownProgress;
    std::unique_ptr<SeparationHelperRun> helper;
};
} // namespace overdub::song
