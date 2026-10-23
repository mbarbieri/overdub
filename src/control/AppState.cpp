#include "control/AppState.h"

#include "capture/ClapMeasurement.h"
#include "capture/LoopbackMeasurement.h"
#include "core/Described.h"
#include "core/Log.h"
#include "song/SongJson.h"
#include "ui/MainComponent.h"
#include "ui/Workspace.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace overdub::control
{
namespace
{
using nlohmann::json;

template <typename T>
json orNull (const std::optional<T>& value)
{
    return value.has_value() ? json (*value) : json (nullptr);
}

template <typename Enum>
std::string nameOf (core::EnumNames<Enum> names, Enum value)
{
    const auto found = std::ranges::find (names, value, &std::pair<Enum, std::string_view>::first);
    return found != names.end() ? std::string (found->second) : std::string();
}

// Every Song op and the ui::Gesture a running activity may refuse it as; nullopt: no activity ever
// refuses it. Exhaustive, so a new Song op does not compile until it is decided here.
struct SongOpGesture
{
    std::string_view op;
    std::optional<ui::Gesture> gesture;
};
constexpr SongOpGesture songOpGestures[] {
    { "move_clip", ui::Gesture::edit },
    { "trim_clip_start", ui::Gesture::edit },
    { "trim_clip_end", ui::Gesture::edit },
    { "set_clip_fade_in", ui::Gesture::edit },
    { "set_clip_fade_out", ui::Gesture::edit },
    { "set_clip_gain", ui::Gesture::edit },
    { "bring_back_pass", ui::Gesture::edit },
    { "set_track_volume", ui::Gesture::edit },
    { "set_track_pan", ui::Gesture::edit },
    { "set_master_volume", ui::Gesture::edit },
    { "set_track_mute", ui::Gesture::edit },
    { "set_track_solo", ui::Gesture::edit },
    { "remove_insert", ui::Gesture::edit },
    { "move_insert", ui::Gesture::edit },
    { "set_insert_bypassed", ui::Gesture::edit },
    { "record", ui::Gesture::record },
    { "play_or_stop", ui::Gesture::playOrStop },
    { "rewind", std::nullopt },
    { "undo", ui::Gesture::undo },
    { "redo", ui::Gesture::redo },
    { "seek", ui::Gesture::seek },
    { "jump_to_pass", ui::Gesture::seek },
    { "nudge", ui::Gesture::seek },
    { "add_track", ui::Gesture::edit },
    { "import_files", ui::Gesture::changeTracks },
    { "delete_track", ui::Gesture::changeTracks },
    { "move_track", ui::Gesture::edit },
    { "set_armed", ui::Gesture::setArmed },
    { "set_input_channels", ui::Gesture::setInputChannels },
    { "set_monitoring", ui::Gesture::setMonitoring },
    { "start_separation", ui::Gesture::startSeparation },
    { "cancel_run", std::nullopt },
    { "set_pre_roll", ui::Gesture::setPreRoll },
    { "set_camera_armed", ui::Gesture::setCameraArmed },
    { "set_camera_device", ui::Gesture::setCameraDevice },
    { "split_selected_clip", ui::Gesture::edit },
    { "delete_selection", ui::Gesture::edit },
    { "take_lane_click", ui::Gesture::edit },
    { "preview_mix", ui::Gesture::previewMix },
    { "add_insert", ui::Gesture::addInsert },
    { "open_insert_editor", std::nullopt },
    { "close_plugin_window", std::nullopt },
    { "set_export_format", std::nullopt },
    { "set_export_length", std::nullopt },
    { "set_export_destination", std::nullopt },
    { "set_export_video_take", std::nullopt },
    { "export", ui::Gesture::startExport },
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
    const auto names = core::operationNames<ui::SongIntent>();
    if (std::size (songOpGestures) != names.size())
        return false;
    return std::ranges::all_of (names, [] (std::string_view op) { return std::ranges::count (songOpGestures, op, &SongOpGesture::op) == 1; });
}
static_assert (std::size (songOpGestures) == core::operationNames<ui::SongIntent>().size(), "songOpGestures must list every Song op");
static_assert (songOpGesturesNameEverySongOpOnce(), "songOpGestures must name each Song op exactly once");

std::optional<ui::Gesture> gestureOf (std::string_view op)
{
    const auto found = std::ranges::find (songOpGestures, op, &SongOpGesture::op);
    return found != std::end (songOpGestures) ? found->gesture : std::nullopt;
}

json songSection (const ui::OpenSong* song)
{
    if (song == nullptr)
        return nullptr;
    return { { "path", song->package().location().getFullPathName().toStdString() }, { "song", song::songToJson (song->song()) } };
}

json historySection (const ui::OpenSong* song)
{
    if (song == nullptr)
        return nullptr;
    const auto& package = song->package();
    return { { "can_undo", package.canUndo() }, { "undo_label", package.undoLabel() }, { "can_redo", package.canRedo() }, { "redo_label", package.redoLabel() } };
}

json transportSection (const ui::OpenSong* song)
{
    if (song == nullptr)
        return nullptr;
    return {
        { "playing", song->isPlaying() },
        { "position", song->position() },
        { "pre_roll_record_point", orNull (song->preRollRecordPoint()) },
        { "running_pass_start", orNull (song->runningPassStart()) },
        { "running_pass_number", song->recordingPassNumber() },
        { "saving_pass", song->isSavingPass() },
    };
}

double activityProgress (const ui::Workspace& workspace, ui::ActivityKind kind)
{
    const auto* song = workspace.openSong();
    if (song == nullptr)
        return 0.0;
    switch (kind)
    {
        case ui::ActivityKind::separation: return song->separationProgress().fraction();
        case ui::ActivityKind::conversion: return song->conversionFraction();
        case ui::ActivityKind::exportSong: return song->exportProgress();
        default: return 0.0;
    }
}

json activitySection (const ui::Workspace& workspace)
{
    const auto kind = workspace.activityKind();
    const auto& log = workspace.activities();
    json lastFinish = nullptr;
    if (const auto& finish = log.lastFinish())
        lastFinish = { { "kind", ui::activityKindName (finish->kind) }, { "counter", finish->counter }, { "error_code", orNull (finish->errorCode) } };
    return { { "kind", ui::activityKindName (kind) }, { "progress", activityProgress (workspace, kind) }, { "counter", log.counter() }, { "last_finish", std::move (lastFinish) } };
}

json timelineView (const ui::SongView& view)
{
    const auto& timeline = view.editPage.timeline;
    auto unfolded = json::array();
    for (const auto& track : view.currentSong().song().tracks)
        if (timeline.areTakeLanesUnfolded (track.id))
            unfolded.push_back (track.id);
    return {
        { "first_sample", timeline.view().firstSample },
        { "samples_per_pixel", timeline.view().samplesPerPixel },
        { "vertical_scroll", timeline.verticalScroll() },
        { "take_lanes_unfolded", std::move (unfolded) },
    };
}

json exportForm (const ui::SongView& view)
{
    const auto request = view.exportPage.request();
    return {
        { "format", nameOf<exporting::ExportFormat> (ui::exportFormatNames, request.format) },
        { "length", nameOf<exporting::VideoLength> (ui::videoLengthNames, request.videoLength) },
        { "destination", request.destination.getFullPathName().toStdString() },
        { "video_take", orNull (request.videoTakeId) },
    };
}

json viewSection (const ui::MainComponent& window)
{
    json view { { "mode", nameOf<ui::Mode> (ui::modeNames, window.mode()) } };
    const auto* songView = window.songView();
    if (songView == nullptr)
        return view;
    const auto& editPage = songView->editPage;
    view["selected_clip"] = orNull (editPage.timeline.selectedClip());
    view["selected_video_take"] = orNull (editPage.timeline.selectedVideoTake());
    view["timeline"] = timelineView (*songView);
    view["mixer_visible"] = editPage.isMixerVisible();
    view["mixer_height"] = editPage.mixerHeight();
    view["passes_panel_open"] = editPage.isPassesPanelOpen();
    view["export_form"] = exportForm (*songView);
    return view;
}

json windowSection (const ui::MainComponent& window)
{
    const auto& dialog = window.runDialog;
    return {
        { "video_preview_wanted", window.isVideoPreviewWanted() },
        { "video_preview_shown", window.isVideoPreviewShown() },
        { "run_dialog",
          { { "showing", dialog.isVisible() },
            { "title", dialog.title.getText().toStdString() },
            { "status", dialog.statusLabel.getText().toStdString() },
            { "progress", dialog.progress() } } },
    };
}

json messageJson (const ui::UserMessage& message)
{
    return { { "code", message.code }, { "tier", ui::tierName (message.tier) }, { "text", ui::sentenceFor (message.code, message.fields) } };
}

json messageListJson (const std::vector<ui::UserMessage>& messages)
{
    auto list = json::array();
    for (const auto& message : messages)
        list.push_back (messageJson (message));
    return list;
}

json messagesSection (const ui::MainComponent& window)
{
    json notice = nullptr;
    if (const auto shown = window.noticeShown())
    {
        notice = messageJson (shown->message);
        notice["counter"] = shown->counter;
        notice["action"] = shown->action.has_value()
                             ? json { { "label", window.noticeLine.actionButton.getButtonText().toStdString() }, { "intent", core::operationToJson (*shown->action) } }
                             : json (nullptr);
        notice["hides_at_ms"] = orNull (window.noticeLine.hidesAtMs());
    }
    return {
        { "notice", std::move (notice) },
        { "alerts_shown", messageListJson (window.alertsShown()) },
        { "alerts_deferred", messageListJson (window.alertsDeferred()) },
    };
}

json audioSection (const ui::Workspace& workspace)
{
    auto& live = workspace.liveAudio();
    const auto choices = live.deviceChoices();
    const auto binding = live.audioBinding();
    const auto* engine = live.engine();
    json problem = nullptr;
    if (const auto* song = workspace.openSong())
    {
        if (const auto songProblem = song->audioProblem())
            problem = songProblem->code;
    }
    else if (engine == nullptr)
        problem = "ui.no_audio_device";
    return {
        { "device_type", choices.current.deviceType },
        { "input_device", choices.current.inputDevice },
        { "output_device", choices.current.outputDevice },
        { "sample_rate", live.sampleRate() },
        { "buffer_size", choices.current.bufferSize },
        { "inputs", binding.inputChannelNames },
        { "outputs", engine != nullptr ? engine->configuration().numOutputChannels : 0 },
        { "audio_problem", std::move (problem) },
    };
}

json errorJson (const core::Error& error)
{
    return { { "code", error.code }, { "fields", error.fields.is_null() ? json::object() : error.fields } };
}

json latencyRunJson (const ui::LatencyRun& run)
{
    return std::visit (
        core::Overloaded {
            [] (const std::monostate&) -> json { return nullptr; },
            [] (const ui::LatencyMeasuring&) -> json { return { { "state", "measuring" } }; },
            [] (const ui::LatencyMeasured& measured) -> json {
        json outcome = measured.outcome.has_value()
                         ? json { { "offset_samples", measured.outcome->offsetSamples }, { "jitter_samples", measured.outcome->jitterSamples }, { "passed", capture::loopbackPassed (*measured.outcome) } }
                         : json { { "error", errorJson (measured.outcome.error()) } };
        return { { "state", "measured" }, { "outcome", std::move (outcome) }, { "to_store", measured.toStore.has_value() ? core::toJson (*measured.toStore) : json (nullptr) } };
    },
        },
        run);
}

json clapRunJson (const ui::ClapRun& run)
{
    return std::visit (
        core::Overloaded {
            [] (const std::monostate&) -> json { return nullptr; },
            [] (const ui::ClapMeasured& measured) -> json {
        json outcome = measured.outcome.has_value()
                         ? json { { "video_take_id", measured.outcome->videoTakeId }, { "camera_id", measured.outcome->cameraId }, { "output_device", measured.outcome->outputDevice }, { "report", capture::clapReportFields (measured.outcome->report) } }
                         : json { { "error", errorJson (measured.outcome.error()) } };
        return { { "state", "measured" }, { "outcome", std::move (outcome) }, { "to_store", measured.toStore.has_value() ? core::toJson (*measured.toStore) : json (nullptr) } };
    },
            [] (const ui::CameraSaved& saved) -> json { return { { "state", "saved" }, { "stored", core::toJson (saved.stored) } }; },
        },
        run);
}

json calibrationSection (const ui::Workspace& workspace)
{
    const auto& state = workspace.calibration();
    const auto& status = state.status;
    return {
        { "device_open", status.deviceOpen },
        { "device_text", status.deviceText },
        { "sample_rate", status.sampleRate },
        { "latency_offset_samples", orNull (status.latencyOffset) },
        { "camera_id", status.cameraId },
        { "camera_name", status.cameraName },
        { "camera_offset_ms", orNull (status.cameraOffsetMs) },
        { "latency_run", latencyRunJson (state.latency) },
        { "clap_run", clapRunJson (state.clap) },
    };
}

json cameraSection (const ui::OpenSong* song)
{
    if (song == nullptr)
        return { { "devices", json::array() }, { "armed", false }, { "chosen", nullptr } };
    auto devices = json::array();
    for (const auto& device : song->cameraDevices())
        devices.push_back ({ { "id", device.id }, { "name", device.name } });
    const auto& chosen = song->song().cameraDeviceId;
    return { { "devices", std::move (devices) }, { "armed", song->song().cameraArmed }, { "chosen", chosen.empty() ? json (nullptr) : json (chosen) } };
}

std::string_view insertStatusName (std::optional<plugin::InsertStatus> status)
{
    if (! status.has_value())
        return "missing";
    switch (*status)
    {
        case plugin::InsertStatus::loading: return "loading";
        case plugin::InsertStatus::ready: return "ready";
        case plugin::InsertStatus::failed: return "failed";
        case plugin::InsertStatus::crashed: return "crashed";
    }
    return "missing";
}

json insertsSection (const ui::OpenSong* song)
{
    auto inserts = json::object();
    if (song == nullptr)
        return inserts;
    const auto add = [&] (const std::vector<song::InsertSlot>& chain) {
        for (const auto& slot : chain)
            inserts[slot.id] = insertStatusName (song->insertStatus (slot.id));
    };
    for (const auto& track : song->song().tracks)
        add (track.insertChain);
    add (song->song().master.insertChain);
    return inserts;
}

json monitoringSection (const ui::OpenSong* song)
{
    auto tracks = json::array();
    if (song != nullptr)
        for (const auto& trackId : song->monitoredTracks())
            tracks.push_back (trackId);
    return tracks;
}

json metersSection (const ui::MainComponent& window)
{
    const auto* view = window.songView();
    if (view == nullptr)
        return { { "tracks", json::object() }, { "inputs", json::object() }, { "master", 0.0 } };
    const auto& peaks = view->shownMeters();
    return { { "tracks", peaks.tracks }, { "inputs", peaks.inputs }, { "master", peaks.master } };
}

template <typename... Alternatives>
void addAppRefusals (json& refused, const ui::Workspace& workspace, std::type_identity<std::variant<Alternatives...>>)
{
    ((refused[std::string (Alternatives::op)] = orNull (workspace.refusalCodeOf (ui::AppIntent { Alternatives {} }))), ...);
}

json refusedSection (const ui::MainComponent& window, const ui::Workspace& workspace)
{
    auto refused = json::object();
    addAppRefusals (refused, workspace, std::type_identity<ui::AppIntent> {});
    const auto runDialogShowing = window.runDialog.isVisible();
    const auto dialogGate = runDialogShowing ? json ("ui.run_dialog_open") : json (nullptr);
    for (const auto op : core::operationNames<ui::WindowIntent>())
        refused[std::string (op)] = dialogGate;

    const auto* song = workspace.openSong();
    for (const auto op : core::operationNames<ui::SongIntent>())
    {
        auto& entry = refused[std::string (op)];
        entry = nullptr;
        if (runDialogShowing && op != ui::intent::CancelRun::op)
            entry = "ui.run_dialog_open";
        else if (const auto gesture = gestureOf (op); gesture.has_value() && song != nullptr)
            if (const auto refusal = song->refusalOf (*gesture))
                entry = refusal->code;
    }
    return refused;
}

json section (std::string_view part, const ui::MainComponent& window, const ui::Workspace& workspace)
{
    const auto* song = workspace.openSong();
    if (part == "song")
        return songSection (song);
    if (part == "history")
        return historySection (song);
    if (part == "transport")
        return transportSection (song);
    if (part == "activity")
        return activitySection (workspace);
    if (part == "idle")
        return workspace.activityKind() == ui::ActivityKind::idle;
    if (part == "view")
        return viewSection (window);
    if (part == "window")
        return windowSection (window);
    if (part == "messages")
        return messagesSection (window);
    if (part == "audio")
        return audioSection (workspace);
    if (part == "calibration")
        return calibrationSection (workspace);
    if (part == "camera")
        return cameraSection (song);
    if (part == "inserts")
        return insertsSection (song);
    if (part == "monitoring")
        return monitoringSection (song);
    if (part == "meters")
        return metersSection (window);
    if (part == "refused")
        return refusedSection (window, workspace);
    OVERDUB_CHECK (false, "control.unknown_app_state_part", { "part", part });
    return nullptr;
}
} // namespace

json appState (const ui::MainComponent& window, const ui::Workspace& workspace, std::span<const std::string_view> parts)
{
    auto state = json::object();
    for (const auto part : parts)
        state[std::string (part)] = section (part, window, workspace);
    return state;
}

StateReader appStateReader (const ui::MainComponent& window, const ui::Workspace& workspace)
{
    return [&window, &workspace] (const json& parts) -> Reply {
        if (parts.is_null())
            return appState (window, workspace);
        const auto badParts = [] {
            return std::unexpected (core::Error { "control.bad_field", "parts is a list of app state sections", { { "key", "parts" }, { "expected", appStateParts } } });
        };
        if (! parts.is_array())
            return badParts();
        std::vector<std::string_view> named;
        for (const auto& part : parts)
        {
            const auto found = part.is_string() ? std::ranges::find (appStateParts, part.get_ref<const std::string&>()) : appStateParts.end();
            if (found == appStateParts.end())
                return badParts();
            named.push_back (*found);
        }
        return appState (window, workspace, named);
    };
}
} // namespace overdub::control
