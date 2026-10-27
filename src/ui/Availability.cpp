#include "ui/Availability.h"

#include "core/Described.h"

#include <algorithm>
#include <array>
#include <string>

namespace overdub::ui
{
namespace
{
using namespace intent;

// Every Song op and the ui::Gesture a running activity may refuse it as; nullopt: no activity ever
// refuses it. Exhaustive, so a new Song op does not compile until it is decided here.
struct SongOpGesture
{
    std::string_view op;
    std::optional<Gesture> gesture;
};
constexpr SongOpGesture songOpGestures[] {
    { "move_clip", Gesture::edit },
    { "trim_clip_start", Gesture::edit },
    { "trim_clip_end", Gesture::edit },
    { "set_clip_fade_in", Gesture::edit },
    { "set_clip_fade_out", Gesture::edit },
    { "set_clip_gain", Gesture::edit },
    { "bring_back_pass", Gesture::edit },
    { "set_track_volume", Gesture::edit },
    { "set_track_pan", Gesture::edit },
    { "set_master_volume", Gesture::edit },
    { "set_track_mute", Gesture::edit },
    { "set_track_solo", Gesture::edit },
    { "remove_insert", Gesture::edit },
    { "move_insert", Gesture::edit },
    { "set_insert_bypassed", Gesture::edit },
    { "record", Gesture::record },
    { "record_again", Gesture::record },
    { "play_or_stop", Gesture::playOrStop },
    { "rewind", std::nullopt },
    { "undo", Gesture::undo },
    { "redo", Gesture::redo },
    { "seek", Gesture::seek },
    { "jump_to_pass", Gesture::seek },
    { "nudge", Gesture::seek },
    { "add_track", Gesture::edit },
    { "import_files", Gesture::changeTracks },
    { "delete_track", Gesture::changeTracks },
    { "move_track", Gesture::edit },
    { "set_armed", Gesture::setArmed },
    { "set_input_channels", Gesture::setInputChannels },
    { "set_monitoring", Gesture::setMonitoring },
    { "start_separation", Gesture::startSeparation },
    { "cancel_run", std::nullopt },
    { "set_transpose", Gesture::edit },
    { "set_pre_roll", Gesture::setPreRoll },
    { "set_camera_armed", Gesture::setCameraArmed },
    { "set_camera_device", Gesture::setCameraDevice },
    { "split_selected_clip", Gesture::edit },
    { "delete_selection", Gesture::edit },
    { "take_lane_click", Gesture::edit },
    { "preview_mix", Gesture::previewMix },
    { "add_insert", Gesture::addInsert },
    { "open_insert_editor", std::nullopt },
    { "close_plugin_window", std::nullopt },
    { "show_export", Gesture::showExport },
    { "set_export_format", std::nullopt },
    { "set_export_length", std::nullopt },
    { "set_export_destination", std::nullopt },
    { "set_export_video_take", std::nullopt },
    { "export", Gesture::startExport },
    { "cancel_export", std::nullopt }, // refused unless an export runs (refusalOfSongOp)
    { "reveal_export", std::nullopt },
    { "select_clip", std::nullopt },
    { "select_video_take", std::nullopt },
    { "set_timeline_view", std::nullopt },
    { "set_vertical_scroll", std::nullopt },
    { "set_take_lanes_unfolded", std::nullopt },
    { "set_mixer_visible", std::nullopt },
    { "set_mixer_height", std::nullopt },
    { "set_passes_panel_open", std::nullopt },
    { "clear_clip_indicator", std::nullopt },
};

constexpr bool songOpGesturesNameEverySongOpOnce()
{
    const auto names = core::operationNames<SongIntent>();
    if (std::size (songOpGestures) != names.size())
        return false;
    return std::ranges::all_of (names, [] (std::string_view op) { return std::ranges::count (songOpGestures, op, &SongOpGesture::op) == 1; });
}
static_assert (std::size (songOpGestures) == core::operationNames<SongIntent>().size(), "songOpGestures must list every Song op");
static_assert (songOpGesturesNameEverySongOpOnce(), "songOpGestures must name each Song op exactly once");

// The Song ops whose refusal is presented as its Notice; the others' controls already show they
// are unavailable (issue #145).
constexpr std::array presentedSongOps { Record::op, RecordAgain::op, Export::op, StartSeparation::op };

// What plays while Transposing is not the Song's Transpose: a Pass or an export would not match it.
constexpr std::array waitForTranspose { Record::op, RecordAgain::op, ShowExport::op, Export::op };

bool isAppOp (std::string_view op)
{
    return std::ranges::contains (core::operationNames<AppIntent>(), op);
}

bool actsUnderRunDialog (std::string_view op)
{
    return op == CancelRun::op || op == ClosePluginWindow::op || isAppOp (op);
}

Refusal appRefusal (std::string_view op, std::string code)
{
    return Refusal { core::Error { std::move (code), "waits until what runs has ended", { { "op", std::string (op) } } }, true };
}

std::optional<Refusal> refusalOfAppOp (std::string_view op, const Running& running)
{
    const auto song = running.song.value_or (ActivityKind::idle);
    if (op == Quit::op)
        return song == ActivityKind::separation ? std::optional (appRefusal (op, "ui.separation_running")) : std::nullopt;
    if (running.calibrating)
        return appRefusal (op, "ui.calibration_running");
    switch (song)
    {
        case ActivityKind::separation: return appRefusal (op, "ui.separation_running");
        case ActivityKind::conversion: return appRefusal (op, "ui.conversion_running");
        case ActivityKind::pass: return appRefusal (op, "ui.stop_recording_first");
        case ActivityKind::exportSong:
            if (op == ConvertSampleRate::op)
                return appRefusal (op, "ui.export_running");
            return std::nullopt;
        case ActivityKind::idle:
        case ActivityKind::calibrationLatency:
        case ActivityKind::calibrationClap: return std::nullopt;
    }
    std::unreachable();
}

std::optional<Refusal> refusalOfSongOp (std::string_view op, const Running& running)
{
    if (op == CancelExport::op && running.song.has_value() && *running.song != ActivityKind::exportSong)
        return Refusal { core::Error { "ui.no_export_running", "no export is running", { { "op", std::string (op) } } }, false };
    if (op == RecordAgain::op && running.songHasNoPass && running.song == ActivityKind::idle)
        return Refusal { core::Error { "ui.no_pass_to_record_again", "the Song has no Pass to record again from", { { "op", std::string (op) } } }, false };
    const auto found = std::ranges::find (songOpGestures, op, &SongOpGesture::op);
    if (! running.song.has_value() || found == std::end (songOpGestures) || ! found->gesture.has_value())
        return std::nullopt;
    const auto presented = std::ranges::contains (presentedSongOps, op);
    if (running.calibrating)
        if (auto refused = refusal (ActivityKind::calibrationLatency, *found->gesture))
            return Refusal { std::move (*refused), presented };
    if (auto refused = refusal (*running.song, *found->gesture))
        return Refusal { std::move (*refused), presented };
    if (running.transposing && std::ranges::contains (waitForTranspose, op))
        return Refusal { core::Error { "ui.transposing", "waits until the Backing Tracks are transposed", { { "op", std::string (op) } } }, presented };
    return std::nullopt;
}
} // namespace

std::optional<Refusal> refusalOf (std::string_view op, const Running& running)
{
    const auto runDialogShowing = running.song == ActivityKind::separation || running.song == ActivityKind::conversion;
    if (runDialogShowing && ! actsUnderRunDialog (op))
        return Refusal { core::Error { "ui.run_dialog_open", "a Separation or a conversion runs", { { "op", std::string (op) } } }, false };
    if (isAppOp (op))
        return refusalOfAppOp (op, running);
    return refusalOfSongOp (op, running);
}

std::optional<Refusal> refusalOf (const Intent& intent, const Running& running)
{
    if (const auto* app = std::get_if<AppIntent> (&intent))
        if (const auto* settings = std::get_if<ShowSettings> (app); settings != nullptr && ! settings->shown)
            return std::nullopt;
    if (const auto* song = std::get_if<SongIntent> (&intent))
        if (const auto* sheet = std::get_if<ShowExport> (song); sheet != nullptr && ! sheet->shown)
            return std::nullopt;
    return refusalOf (opName (intent), running);
}

std::string unavailableReason (std::string_view code)
{
    if (code == "ui.transposing")
        return "Transposing\xe2\x80\xa6";
    if (code == "ui.stop_recording_first")
        return "Stop recording first";
    if (code == "ui.export_running")
        return "Wait for the export to finish";
    return "Wait for what runs to finish";
}
} // namespace overdub::ui
