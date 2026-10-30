#include "ui/Availability.h"

#include "core/Described.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace overdub::ui
{
namespace
{
using namespace intent;

// The gestures some activity refuses, each named in its refusal's field `gesture`. A Separation or
// a conversion is not among them: while either runs, the run dialog's gate answers every Song op
// first (refusalOf). A Pass keeps the Song and the engine's plan as it started. An export renders
// its own plan, so the Song may change, but no Track is imported or deleted under it. A loopback
// latency calibration holds the audio device, so nothing may play or record.
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
    showExport,
    startExport,
    startSeparation,
    setPracticeSpeed,
    setTranscribeMix, // the Focus Track, Transcribe Mix mute and solo (issue #226)
};

struct GestureRule
{
    const char* name;
    bool refusedByPass;
    bool refusedByExport;
    bool refusedByCalibration;
};

constexpr GestureRule ruleFor (Gesture gesture)
{
    switch (gesture)
    {
        // { name, refused by a Pass, by an export, by a calibration }
        case Gesture::record: return { "record", false, true, true };
        case Gesture::playOrStop: return { "play_or_stop", false, true, true };
        case Gesture::seek: return { "seek", true, false, false };
        case Gesture::edit: return { "edit", true, false, false };
        case Gesture::undo: return { "undo", true, false, false };
        case Gesture::redo: return { "redo", true, false, false };
        case Gesture::addInsert: return { "add_insert", true, false, false };
        case Gesture::previewMix: return { "preview_mix", true, false, false };
        case Gesture::setPreRoll: return { "set_pre_roll", true, false, false };
        case Gesture::setArmed: return { "set_armed", true, false, false };
        case Gesture::setInputChannels: return { "set_input_channels", true, false, false };
        case Gesture::setMonitoring: return { "set_monitoring", true, false, true };
        case Gesture::setCameraArmed: return { "set_camera_armed", true, false, false };
        case Gesture::setCameraDevice: return { "set_camera_device", true, false, false };
        case Gesture::changeTracks: return { "change_tracks", true, true, false };
        case Gesture::showExport: return { "show_export", true, true, true };
        case Gesture::startExport: return { "start_export", true, false, true };
        case Gesture::startSeparation: return { "start_separation", true, true, false };
        case Gesture::setPracticeSpeed: return { "set_practice_speed", true, false, false };
        case Gesture::setTranscribeMix: return { "set_transcribe_mix", true, false, false };
    }
    std::unreachable();
}

constexpr std::string_view passRefusal = "ui.stop_recording_first";
constexpr std::string_view exportRefusal = "ui.export_running";
constexpr std::string_view calibrationRefusal = "ui.calibration_running";

// The Error `kind` refuses the gesture with; none when it is allowed.
std::optional<core::Error> activityRefusal (ActivityKind kind, Gesture gesture)
{
    const auto rule = ruleFor (gesture);
    const core::Fields fields { { "gesture", rule.name } };
    if (kind == ActivityKind::pass && rule.refusedByPass)
        return core::Error { std::string (passRefusal), "a Pass is running", fields };
    if (kind == ActivityKind::exportSong && rule.refusedByExport)
        return core::Error { std::string (exportRefusal), "an export is running", fields };
    if (kind == ActivityKind::calibrationLatency && rule.refusedByCalibration)
        return core::Error { std::string (calibrationRefusal), "a latency measurement is running", fields };
    return std::nullopt;
}

// Every Song op and the Gesture a running activity may refuse it as; nullopt: no activity ever
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
    { "move_marker", Gesture::edit },
    { "rename_marker", Gesture::edit },
    { "delete_marker", Gesture::edit },
    { "resize_loop", Gesture::edit },
    { "rename_loop", Gesture::edit },
    { "delete_loop", Gesture::edit },
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
    { "set_practice_speed", Gesture::setPracticeSpeed },
    { "set_focus_track", Gesture::setTranscribeMix },
    { "set_transcribe_mute", Gesture::setTranscribeMix },
    { "set_transcribe_solo", Gesture::setTranscribeMix },
    { "set_octave_shift", Gesture::setTranscribeMix },
    { "set_band_filter", Gesture::setTranscribeMix },
    { "add_marker", Gesture::edit },
    { "select_marker", Gesture::seek },
    { "add_loop", Gesture::edit },
    { "mark_loop_start", Gesture::edit },
    { "mark_loop_end", Gesture::edit },
    { "cancel_loop_start", std::nullopt },
    { "select_loop", Gesture::seek },
    { "select_adjacent_loop", Gesture::seek },
    { "set_looping", Gesture::setTranscribeMix },
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
    { "set_transcribe_view", std::nullopt },
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

// What Transcribe mode refuses: every gesture that changes a Clip, and the arrow keys' nudge.
constexpr std::array refusedWhileTranscribing { MoveClip::op, TrimClipStart::op, TrimClipEnd::op, SetClipFadeIn::op, SetClipFadeOut::op,
                                                SetClipGain::op, SplitSelectedClip::op, DeleteSelection::op, TakeLaneClick::op, Nudge::op };

// What Record mode refuses: adding and editing Markers (issue #227) and Loops (issue #228); its
// strip is read-only.
constexpr std::array refusedInRecordMode { AddMarker::op, MoveMarker::op, RenameMarker::op, DeleteMarker::op,
                                           AddLoop::op, MarkLoopStart::op, MarkLoopEnd::op, ResizeLoop::op, RenameLoop::op, DeleteLoop::op };

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
    if (running.transcribing && running.song.has_value() && std::ranges::contains (refusedWhileTranscribing, op))
        return Refusal { core::Error { "ui.transcribing", "Transcribe mode never changes a Clip", { { "op", std::string (op) } } }, false };
    if (running.recordMode && running.song.has_value() && std::ranges::contains (refusedInRecordMode, op))
        return Refusal { core::Error { "ui.record_mode", "Loops and Markers are made and edited in Transcribe and Edit mode", { { "op", std::string (op) } } }, true };
    const auto found = std::ranges::find (songOpGestures, op, &SongOpGesture::op);
    if (! running.song.has_value() || found == std::end (songOpGestures) || ! found->gesture.has_value())
        return std::nullopt;
    const auto presented = std::ranges::contains (presentedSongOps, op);
    if (running.calibrating)
        if (auto refused = activityRefusal (ActivityKind::calibrationLatency, *found->gesture))
            return Refusal { std::move (*refused), presented };
    if (auto refused = activityRefusal (*running.song, *found->gesture))
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

bool isRefusal (const core::Error& error)
{
    const auto activityCode = error.code == passRefusal || error.code == exportRefusal || error.code == calibrationRefusal;
    return activityCode && error.fields.is_object() && error.fields.contains ("gesture");
}

std::string unavailableReason (std::string_view code)
{
    if (code == "ui.transposing")
        return "Transposing\xe2\x80\xa6";
    if (code == "ui.stop_recording_first")
        return "Stop recording first";
    if (code == "ui.export_running")
        return "Wait for the export to finish";
    if (code == "ui.transcribing")
        return "Unavailable in Transcribe mode";
    if (code == "ui.record_mode")
        return "Unavailable in Record mode";
    return "Wait for what runs to finish";
}
} // namespace overdub::ui
