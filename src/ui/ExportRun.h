#pragma once

#include "core/Error.h"
#include "export/MixExport.h"
#include "export/MovieExport.h"
#include "plugin/InsertHost.h"
#include "song/SongPackage.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <memory>
#include <optional>
#include <set>
#include <vector>

// One export of the open Song (issues #37, #38) and its custody of the Song's plugins (issues #64,
// #65, #78, #103). The plugins are shared between the live plan and the export: the export may run
// them only once the audio thread no longer renders a plan that holds them.
//
// The owner (OpenSong) first hands the engine an empty Song, then creates the ExportRun and calls
// advance() regularly with whether the audio thread has taken that empty plan:
// - Waiting for the audio: once it has let go, the run builds its render from the Song as it is
//   then and renders it on a worker thread (exporting::ExportJob). A slot the Song plays (not
//   bypassed) that is crashed and would still reload discards the export instead: the render would
//   pass it through dry (ui.export_plugin_crashed, path). One that crashed for good is left out, as
//   playback leaves it out. After audioTimeout without the audio letting go, the export is given up
//   (ui.export_audio_unresponsive: path, waited_ms): the device may still call back at any moment,
//   so the plugins are never taken over.
// - Rendering: advance() ends the run once the file is written, with the job's result.
// - crashed() hears the InsertHost's crashes: one in a slot the export renders (while waiting:
//   any slot the Song plays, whatever its load status) discards the export, deleting a file it
//   already wrote. A crash in a slot it does not render leaves it running.
// - followSong() keeps the InsertHost in step with Song edits during the run: a slot an edit
//   removes stays hosted (the export may render it, and only a hosted plugin's crash is heard); a
//   slot an edit adds loads at once and is not rendered.
//
// Every end is returned as an ExportEnd, once; the owner then destroys the run and gives the
// engine the Song again. stop() ends it without an ExportEnd (the owner is detaching the audio).
// Logs ui.export_stopped (info: reason, phase) for every end but a written file and a failed job.
//
// Message thread only. The SongPackage and InsertHost must outlive the run.
namespace overdub::ui
{
enum class ExportPhase
{
    idle,
    waitingForAudio, // the audio thread still renders the Song's plan
    rendering
};

// How an ExportRun ended: the file it wrote, or why it wrote none.
struct ExportEnd
{
    std::optional<juce::File> file;
    std::optional<core::Error> error;
};

class ExportRun
{
public:
    using Clock = song::SongPackage::Clock;

    // How long the run waits for the audio thread to let go of the plugins before it fails.
    static constexpr auto audioTimeout = std::chrono::seconds (5);

    // `now` starts audioTimeout. `movie` is set for an MP4 (its Video Take). `blockSize` is what
    // the render processes per block (the device's, so the plugins stay prepared for it).
    ExportRun (exporting::ExportRequest request,
               std::optional<exporting::MovieSource> movie,
               const song::SongPackage& package,
               plugin::InsertHost& inserts,
               int blockSize,
               Clock::time_point now);
    // Cancels a running render and waits for its worker thread, without logging: an owner that
    // ends the run early calls stop() first.
    ~ExportRun();

    ExportRun (const ExportRun&) = delete;
    ExportRun& operator= (const ExportRun&) = delete;

    // waitingForAudio or rendering (rendering too once the file is written, until advance() ends it).
    ExportPhase phase() const;
    // The fraction written, 0..1 (0 while waiting for the audio).
    double progress() const;
    // True once the render has written its file; the next advance() ends the run.
    bool hasWrittenFile() const;

    // Starts the render once `audioLetGo`, gives the export up after audioTimeout, or ends the run
    // once its file is written. A render started by this call is not ended by it. None while it runs.
    std::optional<ExportEnd> advance (Clock::time_point now, bool audioLetGo);
    // The crashes the InsertHost reported (InsertHost::takeCrashes). An ExportEnd with
    // ui.export_plugin_crashed when the export renders a crashed slot; none (still running) otherwise.
    std::optional<ExportEnd> crashed (const std::vector<plugin::InsertCrash>& crashes);
    // The Song changed: syncs the InsertHost with it, keeping removed slots hosted.
    void followSong();
    // Ends the run for `reason` (logged); the owner destroys it next.
    void stop (const char* reason);

private:
    std::optional<ExportEnd> startRenderOnceAudioLetGo (bool audioLetGo);
    std::optional<ExportEnd> giveUpIfAudioUnresponsive (Clock::time_point now);
    // Whether the export's plan renders this slot (while it waits for the audio: whether the Song
    // plays it, not bypassed, whatever its load status, issue #78).
    bool renders (const song::Id& slotId) const;
    const char* phaseName() const;

    const song::SongPackage& package;
    plugin::InsertHost& inserts;
    int blockSize;
    Clock::time_point requestedAt;
    exporting::ExportRequest request;
    std::optional<exporting::MovieSource> movie; // until the render starts
    std::set<song::Id> renderedSlots;            // with job: the slots its render plays
    std::unique_ptr<exporting::ExportJob> job;   // none while waiting for the audio
};
} // namespace overdub::ui
