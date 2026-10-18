#pragma once

#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "song/SampleRateConversion.h"
#include "song/SeparationRun.h"
#include "ui/ExportRun.h"

#include <memory>
#include <optional>
#include <variant>

// What an OpenSong runs besides playback (issue #145): nothing, a Pass, a Separation, a sample rate
// conversion or an export, never two at once. refusal() is the one table of which gestures each activity refuses.
namespace overdub::ui
{
struct Idle
{
};

struct PassActivity
{
    std::unique_ptr<capture::PassRecorder> recorder;
    int number = 0; // from 1
    // The record point the Pass waits for, when it started with pre-roll.
    std::optional<song::SampleCount> preRollPoint;
    // The Armed camera was unplugged during the Pass: it is disarmed once the Pass is finished.
    bool cameraLost = false;
};

struct SeparationActivity
{
    std::unique_ptr<song::SeparationRun> run;
};

struct ConversionActivity
{
    std::unique_ptr<song::SampleRateConversionRun> run;
};

struct ExportActivity
{
    std::unique_ptr<ExportRun> run;
};

using Activity = std::variant<Idle, PassActivity, SeparationActivity, ConversionActivity, ExportActivity>;

// The OpenSong gestures some activity refuses. Stop, rewind and the export's Video Take choice are
// never refused. Starting the activity that already runs (record during a Pass, startExport during
// an export, startSeparation during a Separation, startSampleRateConversion during a conversion) is
// not refused either: it does nothing.
enum class Gesture
{
    record,
    playOrStop,
    seek,
    edit,
    undo,
    redo,
    addInsert,
    previewMix,
    setPreRoll,
    setArmed,
    setInputChannels,
    setMonitoring,
    setCameraArmed,
    setCameraDevice,
    changeTracks, // import, or delete a Track
    startExport,
    startSeparation,
    startSampleRateConversion,
};

// The Error the running activity refuses the gesture with: ui.stop_recording_first (a Pass),
// ui.separation_running, ui.conversion_running or ui.export_running, with the field `gesture`.
// None when it is allowed.
std::optional<core::Error> refusal (const Activity& activity, Gesture gesture);

// Whether the Error is one refusal() returns (its code, with the field `gesture`).
bool isRefusal (const core::Error& error);
} // namespace overdub::ui
