#pragma once

#include "core/Error.h"
#include "export/ExportFormat.h"
#include "export/MixRender.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

// An Export's work (issue #37): render the mix offline (MixRender) and write it (MixFileWriter)
// to the requested file, faster than real time.
//
// exportMix writes a hidden sibling of the destination first and renames it over the destination
// only once the file is complete, so a failed or cancelled export leaves no partial file and an
// existing file is replaced whole. Logs export.started and export.finished (info).
//
// The destination's folder must exist: an export never creates it (issue #201).
//
// Errors: export.nothing_to_export (the Song has no Clips, whatever its Insert Chains' tails), export.cancelled,
// export.folder_missing (path), export.format_unavailable, export.write_failed (path, step: create |
// client_format | bit_rate | write | close (os_status) | rename (from)).
namespace overdub::exporting
{
struct ExportResult
{
    juce::File file;
    song::SampleCount samples = 0;
};

// Called after each block with the fraction of the Song span written so far (0..1].
using ProgressFn = std::function<void (double fraction)>;
// Asked before each block; true stops the export (export.cancelled).
using ShouldStopFn = std::function<bool()>;

core::Expected<ExportResult> exportMix (MixRender& render, const ExportRequest& request, const ProgressFn& progress = {}, const ShouldStopFn& shouldStop = {});

// Errors: export.folder_missing (path: the destination's folder, which is not a folder).
core::Expected<void> checkExportFolder (const juce::File& destination);
// The destination's folder, created if missing: only the export folder is (ui::SongView).
// Errors: export.write_failed (path, step folder, detail).
core::Expected<void> createExportFolder (const juce::File& destination);
// Renames the complete `partial` over `destination`; on failure deletes it.
// Errors: export.write_failed (step rename).
core::Expected<void> moveExportIntoPlace (const juce::File& partial, const juce::File& destination);

// Renders the whole mix into `file` (no logging, no rename): what exportMix and exportMovie
// share. After the mix ends, silence is written up to `padToSamples` (exportMovie's range end, so
// its audio is as long as its video, even for a Song with no Clips). On failure or a stop the file
// is deleted. Errors: as exportMix's, but folder_missing and rename.
core::Expected<void> writeMixFile (MixRender& render, ExportFormat format, const juce::File& file, const ProgressFn& progress, const ShouldStopFn& shouldStop, song::SampleCount padToSamples = 0);

struct MovieSource; // export/MovieExport.h

// One exportMix (or, given a MovieSource, one exportMovie) on a worker thread. The owner polls progress() and isDone() from its own thread
// and takes the result once done. Destroying the job stops a running export (export.cancelled)
// and waits for its thread.
class ExportJob final : private juce::Thread
{
public:
    ExportJob (std::unique_ptr<MixRender> render, ExportRequest request);
    // An MP4 of `movie`: exportMovie.
    ExportJob (std::unique_ptr<MixRender> render, ExportRequest request, MovieSource movie);
    ~ExportJob() override;

    ExportJob (const ExportJob&) = delete;
    ExportJob& operator= (const ExportJob&) = delete;

    const ExportRequest& request() const { return exportRequest; }
    // The fraction written so far, 0..1.
    double progress() const { return fractionDone.load(); }
    bool isDone() const { return done.load(); }
    // The result once isDone(); none before.
    std::optional<core::Expected<ExportResult>> takeResult();
    // Stops a running export (export.cancelled) and waits for its thread: isDone() after. A file
    // written before the stop was heard is its result.
    void stop();

private:
    void run() override;

    std::unique_ptr<MixRender> render;
    ExportRequest exportRequest;
    std::unique_ptr<MovieSource> movieSource; // none for WAV and M4A
    std::atomic<double> fractionDone { 0.0 };
    std::atomic<bool> done { false };
    std::optional<core::Expected<ExportResult>> result; // written by the worker before `done`
};
} // namespace overdub::exporting
