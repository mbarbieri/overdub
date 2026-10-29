#pragma once

#include "core/Error.h"
#include "ui/Activity.h"
#include "ui/Intent.h"

#include <optional>
#include <string>
#include <string_view>

// Which Intents may run now (issue #195): the one answer MainComponent::run checks before it
// dispatches, and every control, menu item and dialog opener reads to show itself unavailable.
//
// While a Separation or a conversion runs, its modal run dialog lets only cancel_run,
// close_plugin_window and the App group through (ui.run_dialog_open, field `op`, logged at debug).
// The App group, but quit and hiding the Settings window, waits until nothing runs but an export
// (ui.calibration_running, ui.separation_running, ui.conversion_running or ui.stop_recording_first,
// field `op`, presented as its Notice); convert_sample_rate also waits for an export, and quit
// only for a Separation. A Song op is refused as its ui::Gesture is (ui::refusal, field `gesture`):
// presented for record, export and start_separation, logged at debug for the others, whose
// controls already show they are unavailable: show_export, which opens the Export sheet, waits until
// nothing runs (issue #200), and cancel_export waits for an export (ui.no_export_running, field
// `op`, issue #202). In Transcribe mode the Clip edits and nudge are refused (ui.transcribing, field
// `op`, logged at debug, issue #225): Transcribe mode is for listening and never changes a Clip.
// In Record mode adding, moving, renaming and deleting a Marker are refused (ui.record_mode, field
// `op`, presented as its Notice, issue #227): Record shows the Markers read-only.
// Window ops other than under the run dialog are never refused.
namespace overdub::ui
{
// What runs in the app, as the refusals read it.
struct Running
{
    // The open Song's activity; none while no Song is open, when no Song op is refused (each
    // answers ui.no_song_open).
    std::optional<ActivityKind> song;
    // A loopback latency calibration (the Workspace's), which runs beside an export only.
    bool calibrating = false;
    // The open Song has no Pass: record_again has no record point while no Pass runs either
    // (ui.no_pass_to_record_again, field `op`, logged at debug, issue #216).
    bool songHasNoPass = false;
    // The open Song's playback is not at its Transpose yet (OpenSong::isTransposing): record,
    // record_again and export wait for it (ui.transposing, field `op`, presented as its Notice), and
    // so does show_export (logged at debug) (issues #209, #210).
    bool transposing = false;
    // The window shows Transcribe mode: the Clip edits and nudge are refused (ui.transcribing).
    bool transcribing = false;
    // The window shows Record mode: the Marker edits are refused (ui.record_mode).
    bool recordMode = false;
};

struct Refusal
{
    core::Error error;
    // Shown as its Notice; otherwise logged at debug.
    bool presented = false;
};

// The refusal of the op with its default fields (show_settings shows the window), none when it
// may run.
std::optional<Refusal> refusalOf (std::string_view op, const Running& running);
// The refusal of this very Intent: hiding the Settings window or the Export sheet is never refused.
std::optional<Refusal> refusalOf (const Intent& intent, const Running& running);

// Why a control is unavailable (its tooltip), from the code its op is refused with.
std::string unavailableReason (std::string_view code);
} // namespace overdub::ui
