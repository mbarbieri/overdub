#pragma once

#include "control/Controller.h"

#include <nlohmann/json.hpp>

#include <array>
#include <span>
#include <string_view>

namespace overdub::ui
{
class MainComponent;
class Workspace;
} // namespace overdub::ui

// The app state (issue #167, ADR 0008): one JSON object of everything a verifier can inspect,
// read from the owners' public getters. Reading it changes nothing: no meter peak is taken (meters
// are the values the UI last showed), nothing is logged, no dialog opens. Message thread only.
//
// Sections, each a key of the object:
//   song         null, or {"path": the package folder, "song": song::songToJson}
//   history      null, or {"can_undo", "undo_label", "can_redo", "redo_label"}
//   transport    null, or {"playing", "position", "pre_roll_record_point", "running_pass_start",
//                "running_pass_number" (0 when none), "saving_pass",
//                "heard_transpose", "transposing", "transpose_rendering" (issue #209),
//                "practice_speed", "heard_practice_speed", "practice_rendering" (issue #212),
//                "transcribe_mix_heard" (issue #226: the engine plays the Transcribe Mix)}
//   activity     {"kind", "progress" 0..1, "counter", "last_finish": null | {"kind", "counter",
//                "error_code"}}; kind is ui::activityKindNames'; counter counts the activities
//                started, so it names the running or last one
//   idle         activity.kind is "idle" (an export or a calibration run is not idle)
//   view         {"mode"} and, with a Song open, "selected_clip", "selected_video_take",
//                "selected_marker" (issue #227),
//                "timeline" {"first_sample", "samples_per_pixel", "vertical_scroll",
//                "take_lanes_unfolded": [track ids]}, "mixer_visible", "mixer_height",
//                "passes_panel_open", "export_sheet_shown", "export_form" {"format", "length",
//                "destination", "video_take", "replaces_existing"}
//   window       {"video_preview_shown", "run_dialog": {"showing",
//                "title", "status", "progress"}}
//   messages     {"notice", "alerts_shown", "alerts_deferred"}; each message is {"code", "tier",
//                "text"}. notice is null while none shows, else also "counter" (the Notices shown
//                up to it, so the same one shown again is seen), "action": null | {"label",
//                "intent": the Intent its button runs, as the socket takes it} and "hides_at_ms":
//                when it hides on its own (Unix epoch ms, fixed when shown), null while it has
//                an action (it shows until clicked or replaced). alerts_shown:
//                the open Alerts; alerts_deferred: those a running Pass holds back; oldest first
//   audio        {"device_type", "input_device", "output_device", "sample_rate", "buffer_size",
//                "inputs": [active input channel names], "outputs": output channels,
//                "audio_problem": null | code}
//   calibration  the Workspace's CalibrationState
//   camera       {"devices": [{"id", "name"}], "armed", "chosen"}; empty without a Song
//   inserts      {slot id: "loading" | "ready" | "failed" | "crashed" | "missing"}
//   monitoring   [track ids]
//   meters       {"tracks": {track id: peak}, "inputs": {track id: peak}, "master"}
//   refused      {op: the code that op is refused with now, or null}: the run dialog's
//                (ui.run_dialog_open), the Workspace's (App Intents while something runs, and
//                convert_sample_rate under the open Song's activity) and the running activity's
//                (ui/Activity.h refusal())
namespace overdub::control
{
inline constexpr std::array<std::string_view, 15> appStateParts {
    "song",
    "history",
    "transport",
    "activity",
    "idle",
    "view",
    "window",
    "messages",
    "audio",
    "calibration",
    "camera",
    "inserts",
    "monitoring",
    "meters",
    "refused",
};

// The sections named in `parts` (each one of appStateParts).
nlohmann::json appState (const ui::MainComponent& window, const ui::Workspace& workspace, std::span<const std::string_view> parts = appStateParts);

// The Controller's `state` reader: `parts` null reads every section, else it is a list of section
// names (control.bad_field with key "parts" for anything else). Both must outlive it.
StateReader appStateReader (const ui::MainComponent& window, const ui::Workspace& workspace);
} // namespace overdub::control
