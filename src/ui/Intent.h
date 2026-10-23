#pragma once

#include "core/Described.h"
#include "core/Error.h"
#include "export/ExportFormat.h"
#include "export/MovieSpan.h"
#include "song/Song.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// The vocabulary of user operations (ADR 0008): every gesture, once its dialog (if any) has been
// answered, is one Intent run through MainComponent::run, and the control socket parses the same
// Intents from JSON (core/Described.h) and runs them through the same MainComponent::run.
//
// - An Intent is in domain terms (ids, samples, dB), never pixels. Gesture phases (drag frames,
//   Esc during a drag, arrow presses in a burst, hover) are not operations; what they commit is.
// - A dialog collects an Intent's fields: ImportFiles carries the chosen files, DeleteTrack is the
//   confirmed delete. The handler, not the dialog, owns every check, because the answer may arrive
//   later, from the socket, or after the Song changed.
// - Toggles are set-to-value (SetMixerVisible { true }), so running an Intent twice is harmless.
//
// The three groups are the three owners: AppIntent (the app's File and settings operations),
// WindowIntent (ui::MainComponent) and SongIntent (ui::SongView, refused with ui.no_song_open while
// no Song is open). SongIntent nests intent::Edit, the plain one-undo-step edits (ui/SongEdits.h).
// Each owner visits its group with core::visitExhaustively, so an alternative without a handler does
// not compile, and a struct without `op` and `fields` cannot be an alternative.
//
// Keys (issue #9 resolution, issues #27, #28, #128): 1-3 pick a mode, R records, Space plays or
// stops, command-Z / shift-command-Z undo and redo, and S, backspace (or forward delete) and the
// left and right arrows split, delete and nudge, which their handlers refuse outside Edit mode.
namespace overdub::ui
{
enum class Mode
{
    record,
    edit,
    exportSong
};
inline constexpr std::pair<Mode, std::string_view> modeNames[] { { Mode::record, "record" }, { Mode::edit, "edit" }, { Mode::exportSong, "export" } };

// "record", "edit" or "export": the name in ui.* event fields and in the Intent's JSON.
const char* modeName (Mode mode);

enum class Direction
{
    back,
    forward
};
inline constexpr std::pair<Direction, std::string_view> directionNames[] { { Direction::back, "back" }, { Direction::forward, "forward" } };

// How far Nudge moves the playhead.
inline constexpr int nudgeSeconds = 5;

inline constexpr std::pair<song::PreRoll, std::string_view> preRollNames[] {
    { song::PreRoll::off, "off" },
    { song::PreRoll::oneSecond, "one_second" },
    { song::PreRoll::twoSeconds, "two_seconds" },
    { song::PreRoll::fourSeconds, "four_seconds" },
};
inline constexpr std::pair<exporting::ExportFormat, std::string_view> exportFormatNames[] {
    { exporting::ExportFormat::wav, "wav" },
    { exporting::ExportFormat::m4a, "m4a" },
    { exporting::ExportFormat::mp4, "mp4" },
};
inline constexpr std::pair<exporting::VideoLength, std::string_view> videoLengthNames[] {
    { exporting::VideoLength::trimToVideoTake, "trim_to_video_take" },
    { exporting::VideoLength::fullSong, "full_song" },
};

namespace intent
{
using core::enumField;
using core::field;

//==============================================================================
// App: the open Song's lifecycle, the device, calibration, the app.

struct NewSongAt // File > New Song..., after the save dialog
{
    static constexpr std::string_view op = "new_song_at";
    core::AbsolutePath path;
    bool operator== (const NewSongAt&) const = default;
    static constexpr auto fields = std::tuple { field ("path", &NewSongAt::path) };
};

struct OpenSongAt // File > Open..., and the no-Song page's Open
{
    static constexpr std::string_view op = "open_song_at";
    core::AbsolutePath path;
    bool operator== (const OpenSongAt&) const = default;
    static constexpr auto fields = std::tuple { field ("path", &OpenSongAt::path) };
};

struct DuplicateSongTo // File > Duplicate...
{
    static constexpr std::string_view op = "duplicate_song_to";
    core::AbsolutePath path;
    bool operator== (const DuplicateSongTo&) const = default;
    static constexpr auto fields = std::tuple { field ("path", &DuplicateSongTo::path) };
};

struct ConvertSampleRate // File > Convert Sample Rate > rate, and the ui.sample_rate_mismatch Notice's action
{
    static constexpr std::string_view op = "convert_sample_rate";
    int rate = 0;
    bool operator== (const ConvertSampleRate&) const = default;
    static constexpr auto fields = std::tuple { field ("rate", &ConvertSampleRate::rate) };
};

struct ShowSettings // Overdub > Settings..., and the Settings window's close button
{
    static constexpr std::string_view op = "show_settings";
    bool shown = true;
    bool operator== (const ShowSettings&) const = default;
    static constexpr auto fields = std::tuple { field ("shown", &ShowSettings::shown) };
};

struct ApplyDeviceSettings // the Settings window's device pickers
{
    static constexpr std::string_view op = "apply_device_settings";
    std::string deviceType;
    std::string inputDevice;
    std::string outputDevice;
    int sampleRate = 0;
    int bufferSize = 0;
    bool operator== (const ApplyDeviceSettings&) const = default;
    static constexpr auto fields = std::tuple { field ("device_type", &ApplyDeviceSettings::deviceType),
                                                field ("input_device", &ApplyDeviceSettings::inputDevice),
                                                field ("output_device", &ApplyDeviceSettings::outputDevice),
                                                field ("sample_rate", &ApplyDeviceSettings::sampleRate),
                                                field ("buffer_size", &ApplyDeviceSettings::bufferSize) };
};

struct MeasureLatency
{
    static constexpr std::string_view op = "measure_latency";
    bool operator== (const MeasureLatency&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct SaveLatencyCalibration
{
    static constexpr std::string_view op = "save_latency_calibration";
    song::SampleCount offsetSamples = 0;
    bool operator== (const SaveLatencyCalibration&) const = default;
    static constexpr auto fields = std::tuple { field ("offset_samples", &SaveLatencyCalibration::offsetSamples) };
};

struct MeasureClapPass
{
    static constexpr std::string_view op = "measure_clap_pass";
    bool operator== (const MeasureClapPass&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct SaveCameraCalibration
{
    static constexpr std::string_view op = "save_camera_calibration";
    std::string cameraId;
    std::string outputDevice;
    double offsetMs = 0.0;
    bool operator== (const SaveCameraCalibration&) const = default;
    static constexpr auto fields = std::tuple { field ("camera_id", &SaveCameraCalibration::cameraId),
                                                field ("output_device", &SaveCameraCalibration::outputDevice),
                                                field ("offset_ms", &SaveCameraCalibration::offsetMs) };
};

struct Quit // the window's close button, command-Q
{
    static constexpr std::string_view op = "quit";
    bool operator== (const Quit&) const = default;
    static constexpr auto fields = std::tuple {};
};

//==============================================================================
// Window: what the main window shows besides the Song. Needs no Song.

struct ShowMode // keys 1-3, the top bar's mode buttons
{
    static constexpr std::string_view op = "show_mode";
    Mode mode = Mode::record;
    bool operator== (const ShowMode&) const = default;
    static constexpr auto fields = std::tuple { enumField ("mode", &ShowMode::mode, core::EnumNames<Mode> { modeNames }) };
};

struct SetVideoPreviewWanted // the Edit toolbar's Video toggle, the Video Preview's close button
{
    static constexpr std::string_view op = "set_video_preview_wanted";
    bool wanted = true;
    bool operator== (const SetVideoPreviewWanted&) const = default;
    static constexpr auto fields = std::tuple { field ("wanted", &SetVideoPreviewWanted::wanted) };
};

struct DismissAlerts // OK on every open Alert
{
    static constexpr std::string_view op = "dismiss_alerts";
    bool operator== (const DismissAlerts&) const = default;
    static constexpr auto fields = std::tuple {};
};

//==============================================================================
// Song: plain edits, one undo step each through SongView's one edit funnel and nothing else.
// Anything with a follow-up (a selection, Monitoring, a log, focus) is a Song alternative of its own.

struct MoveClip
{
    static constexpr std::string_view op = "move_clip";
    song::Id clipId;
    song::SampleCount start = 0;
    bool operator== (const MoveClip&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &MoveClip::clipId), field ("start", &MoveClip::start) };
};

struct TrimClipStart
{
    static constexpr std::string_view op = "trim_clip_start";
    song::Id clipId;
    song::SampleCount start = 0;
    bool operator== (const TrimClipStart&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &TrimClipStart::clipId), field ("start", &TrimClipStart::start) };
};

struct TrimClipEnd
{
    static constexpr std::string_view op = "trim_clip_end";
    song::Id clipId;
    song::SampleCount end = 0;
    bool operator== (const TrimClipEnd&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &TrimClipEnd::clipId), field ("end", &TrimClipEnd::end) };
};

struct SetClipFadeIn // a fade handle drag, or the toolbar's Fade In field
{
    static constexpr std::string_view op = "set_clip_fade_in";
    song::Id clipId;
    song::SampleCount length = 0;
    bool operator== (const SetClipFadeIn&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &SetClipFadeIn::clipId), field ("length", &SetClipFadeIn::length) };
};

struct SetClipFadeOut
{
    static constexpr std::string_view op = "set_clip_fade_out";
    song::Id clipId;
    song::SampleCount length = 0;
    bool operator== (const SetClipFadeOut&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &SetClipFadeOut::clipId), field ("length", &SetClipFadeOut::length) };
};

struct SetClipGain
{
    static constexpr std::string_view op = "set_clip_gain";
    song::Id clipId;
    double gainDb = 0.0;
    bool operator== (const SetClipGain&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &SetClipGain::clipId), field ("gain_db", &SetClipGain::gainDb) };
};

struct BringBackPass // ↺ on a Pass row
{
    static constexpr std::string_view op = "bring_back_pass";
    song::Id passId;
    bool operator== (const BringBackPass&) const = default;
    static constexpr auto fields = std::tuple { field ("pass_id", &BringBackPass::passId) };
};

struct SetTrackVolume // a Mixer or Record row fader: drag end, double-click, a committed arrow burst
{
    static constexpr std::string_view op = "set_track_volume";
    song::Id trackId;
    double volumeDb = 0.0;
    bool operator== (const SetTrackVolume&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTrackVolume::trackId), field ("volume_db", &SetTrackVolume::volumeDb) };
};

struct SetTrackPan
{
    static constexpr std::string_view op = "set_track_pan";
    song::Id trackId;
    double pan = 0.0;
    bool operator== (const SetTrackPan&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTrackPan::trackId), field ("pan", &SetTrackPan::pan) };
};

struct SetMasterVolume
{
    static constexpr std::string_view op = "set_master_volume";
    double volumeDb = 0.0;
    bool operator== (const SetMasterVolume&) const = default;
    static constexpr auto fields = std::tuple { field ("volume_db", &SetMasterVolume::volumeDb) };
};

struct SetTrackMute // M on a Record row, an Edit header or a Mixer strip
{
    static constexpr std::string_view op = "set_track_mute";
    song::Id trackId;
    bool mute = true;
    bool operator== (const SetTrackMute&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTrackMute::trackId), field ("mute", &SetTrackMute::mute) };
};

struct SetTrackSolo
{
    static constexpr std::string_view op = "set_track_solo";
    song::Id trackId;
    bool solo = true;
    bool operator== (const SetTrackSolo&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTrackSolo::trackId), field ("solo", &SetTrackSolo::solo) };
};

struct RemoveInsert
{
    static constexpr std::string_view op = "remove_insert";
    song::Id slotId;
    bool operator== (const RemoveInsert&) const = default;
    static constexpr auto fields = std::tuple { field ("slot_id", &RemoveInsert::slotId) };
};

struct MoveInsert
{
    static constexpr std::string_view op = "move_insert";
    song::Id slotId;
    std::size_t toIndex = 0;
    bool operator== (const MoveInsert&) const = default;
    static constexpr auto fields = std::tuple { field ("slot_id", &MoveInsert::slotId), field ("to_index", &MoveInsert::toIndex) };
};

struct SetInsertBypassed // the chip's bypass, and the plugin window's
{
    static constexpr std::string_view op = "set_insert_bypassed";
    song::Id slotId;
    bool bypassed = true;
    bool operator== (const SetInsertBypassed&) const = default;
    static constexpr auto fields = std::tuple { field ("slot_id", &SetInsertBypassed::slotId), field ("bypassed", &SetInsertBypassed::bypassed) };
};

using Edit = std::variant<MoveClip, TrimClipStart, TrimClipEnd, SetClipFadeIn, SetClipFadeOut, SetClipGain, BringBackPass,
                          SetTrackVolume, SetTrackPan, SetMasterVolume, SetTrackMute, SetTrackSolo,
                          RemoveInsert, MoveInsert, SetInsertBypassed>;

// The edits a Mixer gesture in progress can be heard with (OpenSong::previewMix).
using MixEdit = std::variant<SetTrackVolume, SetTrackPan, SetMasterVolume>;

//==============================================================================
// Song: transport and history.

struct Record // R, the ● button
{
    static constexpr std::string_view op = "record";
    bool operator== (const Record&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct PlayOrStop // Space, the ▶/■ button
{
    static constexpr std::string_view op = "play_or_stop";
    bool operator== (const PlayOrStop&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct Rewind // ⏮
{
    static constexpr std::string_view op = "rewind";
    bool operator== (const Rewind&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct Undo
{
    static constexpr std::string_view op = "undo";
    bool operator== (const Undo&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct Redo
{
    static constexpr std::string_view op = "redo";
    bool operator== (const Redo&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct Seek // a click on the Edit ruler or the Record overview
{
    static constexpr std::string_view op = "seek";
    static constexpr bool runsPerFrame = true;
    song::SampleCount sample = 0;
    bool operator== (const Seek&) const = default;
    static constexpr auto fields = std::tuple { field ("sample", &Seek::sample) };
};

struct JumpToPass // a click on a Record Pass row
{
    static constexpr std::string_view op = "jump_to_pass";
    song::Id passId;
    bool operator== (const JumpToPass&) const = default;
    static constexpr auto fields = std::tuple { field ("pass_id", &JumpToPass::passId) };
};

struct Nudge // ← and → in Edit mode
{
    static constexpr std::string_view op = "nudge";
    Direction direction = Direction::forward;
    bool operator== (const Nudge&) const = default;
    static constexpr auto fields = std::tuple { enumField ("direction", &Nudge::direction, core::EnumNames<Direction> { directionNames }) };
};

//==============================================================================
// Song: Tracks.

struct AddTrack
{
    static constexpr std::string_view op = "add_track";
    bool operator== (const AddTrack&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct ImportFiles // Import... after its chooser, or files dropped on the Timeline
{
    static constexpr std::string_view op = "import_files";
    std::vector<core::AbsolutePath> files;
    bool operator== (const ImportFiles&) const = default;
    static constexpr auto fields = std::tuple { field ("files", &ImportFiles::files) };
};

struct DeleteTrack // × on an Edit header, after Delete in its confirmation
{
    static constexpr std::string_view op = "delete_track";
    song::Id trackId;
    bool operator== (const DeleteTrack&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &DeleteTrack::trackId) };
};

struct MoveTrack // a grip drop
{
    static constexpr std::string_view op = "move_track";
    song::Id trackId;
    std::size_t toIndex = 0;
    bool operator== (const MoveTrack&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &MoveTrack::trackId), field ("to_index", &MoveTrack::toIndex) };
};

struct SetArmed
{
    static constexpr std::string_view op = "set_armed";
    song::Id trackId;
    bool armed = true;
    bool operator== (const SetArmed&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetArmed::trackId), field ("armed", &SetArmed::armed) };
};

struct SetInputChannels
{
    static constexpr std::string_view op = "set_input_channels";
    song::Id trackId;
    std::vector<int> channels;
    bool operator== (const SetInputChannels&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetInputChannels::trackId), field ("channels", &SetInputChannels::channels) };
};

struct SetMonitoring
{
    static constexpr std::string_view op = "set_monitoring";
    song::Id trackId;
    bool on = true;
    bool operator== (const SetMonitoring&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetMonitoring::trackId), field ("on", &SetMonitoring::on) };
};

struct StartSeparation // Separate... in a Record row's menu
{
    static constexpr std::string_view op = "start_separation";
    song::Id trackId;
    bool operator== (const StartSeparation&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &StartSeparation::trackId) };
};

struct CancelRun // the run dialog's Cancel (a Separation or a conversion)
{
    static constexpr std::string_view op = "cancel_run";
    bool operator== (const CancelRun&) const = default;
    static constexpr auto fields = std::tuple {};
};

//==============================================================================
// Song: Record settings.

struct SetPreRoll
{
    static constexpr std::string_view op = "set_pre_roll";
    song::PreRoll preRoll = song::PreRoll::off;
    bool operator== (const SetPreRoll&) const = default;
    static constexpr auto fields = std::tuple { enumField ("pre_roll", &SetPreRoll::preRoll, core::EnumNames<song::PreRoll> { preRollNames }) };
};

struct SetCameraArmed // arming asks for camera access first (the system's prompt)
{
    static constexpr std::string_view op = "set_camera_armed";
    bool armed = true;
    bool operator== (const SetCameraArmed&) const = default;
    static constexpr auto fields = std::tuple { field ("armed", &SetCameraArmed::armed) };
};

struct SetCameraDevice
{
    static constexpr std::string_view op = "set_camera_device";
    std::string cameraId;
    bool operator== (const SetCameraDevice&) const = default;
    static constexpr auto fields = std::tuple { field ("camera_id", &SetCameraDevice::cameraId) };
};

//==============================================================================
// Song: Clips by selection and Take Lanes; the handler resolves the target.

struct SplitSelectedClip // S, Split: at the playhead, then the right half is selected
{
    static constexpr std::string_view op = "split_selected_clip";
    bool operator== (const SplitSelectedClip&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct DeleteSelection // backspace, Delete: the selected Video Take, else the selected Clip
{
    static constexpr std::string_view op = "delete_selection";
    bool operator== (const DeleteSelection&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct TakeLaneClick // uses the Take over the Clip there, or brings the Take back (ui/TakeLanes.h laneEditAt)
{
    static constexpr std::string_view op = "take_lane_click";
    song::Id takeId;
    song::SampleCount at = 0;
    bool operator== (const TakeLaneClick&) const = default;
    static constexpr auto fields = std::tuple { field ("take_id", &TakeLaneClick::takeId), field ("at", &TakeLaneClick::at) };
};

// A Mixer gesture in progress, heard and not committed; none: hear the Song as it is.
struct PreviewMix
{
    static constexpr std::string_view op = "preview_mix";
    static constexpr bool runsPerFrame = true;
    std::optional<MixEdit> edit;
    bool operator== (const PreviewMix&) const = default;
    static constexpr auto fields = std::tuple { field ("edit", &PreviewMix::edit) };
};

//==============================================================================
// Song: Inserts.

struct AddInsert // "+ Insert", after picking an AU effect
{
    static constexpr std::string_view op = "add_insert";
    song::Id trackId; // empty: the Master's chain
    std::string type;
    std::string subtype;
    std::string manufacturer;
    bool operator== (const AddInsert&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &AddInsert::trackId),
                                                field ("type", &AddInsert::type),
                                                field ("subtype", &AddInsert::subtype),
                                                field ("manufacturer", &AddInsert::manufacturer) };
};

struct OpenInsertEditor // a chip click
{
    static constexpr std::string_view op = "open_insert_editor";
    song::Id slotId;
    bool operator== (const OpenInsertEditor&) const = default;
    static constexpr auto fields = std::tuple { field ("slot_id", &OpenInsertEditor::slotId) };
};

struct ClosePluginWindow // captures the plugin's state into song.json, outside history
{
    static constexpr std::string_view op = "close_plugin_window";
    song::Id slotId;
    bool operator== (const ClosePluginWindow&) const = default;
    static constexpr auto fields = std::tuple { field ("slot_id", &ClosePluginWindow::slotId) };
};

//==============================================================================
// Song: Export. The form is the Export page's; Export starts what the form says.

struct SetExportFormat
{
    static constexpr std::string_view op = "set_export_format";
    exporting::ExportFormat format = exporting::ExportFormat::wav;
    bool operator== (const SetExportFormat&) const = default;
    static constexpr auto fields = std::tuple { enumField ("format", &SetExportFormat::format, core::EnumNames<exporting::ExportFormat> { exportFormatNames }) };
};

struct SetExportLength
{
    static constexpr std::string_view op = "set_export_length";
    exporting::VideoLength length = exporting::VideoLength::trimToVideoTake;
    bool operator== (const SetExportLength&) const = default;
    static constexpr auto fields = std::tuple { enumField ("length", &SetExportLength::length, core::EnumNames<exporting::VideoLength> { videoLengthNames }) };
};

struct SetExportDestination // Choose..., after the save dialog
{
    static constexpr std::string_view op = "set_export_destination";
    core::AbsolutePath path;
    bool operator== (const SetExportDestination&) const = default;
    static constexpr auto fields = std::tuple { field ("path", &SetExportDestination::path) };
};

struct SetExportVideoTake // stored in the Song, outside history
{
    static constexpr std::string_view op = "set_export_video_take";
    song::Id videoTakeId;
    bool operator== (const SetExportVideoTake&) const = default;
    static constexpr auto fields = std::tuple { field ("video_take_id", &SetExportVideoTake::videoTakeId) };
};

struct Export
{
    static constexpr std::string_view op = "export";
    bool operator== (const Export&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct RevealExport // Reveal in Finder: the file the last export wrote
{
    static constexpr std::string_view op = "reveal_export";
    bool operator== (const RevealExport&) const = default;
    static constexpr auto fields = std::tuple {};
};

//==============================================================================
// Song: the view. Transient, never in the Song, but state the user changes and sees.

struct SelectClip // a click on a Clip; none: a click on empty lane space
{
    static constexpr std::string_view op = "select_clip";
    std::optional<song::Id> clipId;
    bool operator== (const SelectClip&) const = default;
    static constexpr auto fields = std::tuple { field ("clip_id", &SelectClip::clipId) };
};

struct SelectVideoTake // a click on a Camera lane block
{
    static constexpr std::string_view op = "select_video_take";
    std::optional<song::Id> videoTakeId;
    bool operator== (const SelectVideoTake&) const = default;
    static constexpr auto fields = std::tuple { field ("video_take_id", &SelectVideoTake::videoTakeId) };
};

struct SetTimelineView // zoom, pinch and horizontal scroll; clamped as Timeline::setView clamps
{
    static constexpr std::string_view op = "set_timeline_view";
    static constexpr bool runsPerFrame = true;
    song::SampleCount firstSample = 0;
    song::SampleCount samplesPerPixel = 0;
    bool operator== (const SetTimelineView&) const = default;
    static constexpr auto fields = std::tuple { field ("first_sample", &SetTimelineView::firstSample),
                                                field ("samples_per_pixel", &SetTimelineView::samplesPerPixel) };
};

struct SetVerticalScroll
{
    static constexpr std::string_view op = "set_vertical_scroll";
    static constexpr bool runsPerFrame = true;
    int pixels = 0;
    bool operator== (const SetVerticalScroll&) const = default;
    static constexpr auto fields = std::tuple { field ("pixels", &SetVerticalScroll::pixels) };
};

struct SetTakeLanesUnfolded
{
    static constexpr std::string_view op = "set_take_lanes_unfolded";
    song::Id trackId;
    bool unfolded = true;
    bool operator== (const SetTakeLanesUnfolded&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTakeLanesUnfolded::trackId), field ("unfolded", &SetTakeLanesUnfolded::unfolded) };
};

struct SetMixerVisible
{
    static constexpr std::string_view op = "set_mixer_visible";
    bool visible = true;
    bool operator== (const SetMixerVisible&) const = default;
    static constexpr auto fields = std::tuple { field ("visible", &SetMixerVisible::visible) };
};

struct SetMixerHeight // the divider drag
{
    static constexpr std::string_view op = "set_mixer_height";
    static constexpr bool runsPerFrame = true;
    int height = 0;
    bool operator== (const SetMixerHeight&) const = default;
    static constexpr auto fields = std::tuple { field ("height", &SetMixerHeight::height) };
};

struct SetPassesPanelOpen
{
    static constexpr std::string_view op = "set_passes_panel_open";
    bool open = true;
    bool operator== (const SetPassesPanelOpen&) const = default;
    static constexpr auto fields = std::tuple { field ("open", &SetPassesPanelOpen::open) };
};

struct ClearClipIndicator // a click on a Record row's clip indicator
{
    static constexpr std::string_view op = "clear_clip_indicator";
    song::Id trackId;
    bool operator== (const ClearClipIndicator&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &ClearClipIndicator::trackId) };
};
} // namespace intent

using AppIntent = std::variant<intent::NewSongAt, intent::OpenSongAt, intent::DuplicateSongTo, intent::ConvertSampleRate,
                               intent::ShowSettings, intent::ApplyDeviceSettings, intent::MeasureLatency,
                               intent::SaveLatencyCalibration, intent::MeasureClapPass, intent::SaveCameraCalibration,
                               intent::Quit>;

using WindowIntent = std::variant<intent::ShowMode, intent::SetVideoPreviewWanted, intent::DismissAlerts>;

using SongIntent = std::variant<intent::Edit,
                                intent::Record, intent::PlayOrStop, intent::Rewind, intent::Undo, intent::Redo,
                                intent::Seek, intent::JumpToPass, intent::Nudge,
                                intent::AddTrack, intent::ImportFiles, intent::DeleteTrack, intent::MoveTrack,
                                intent::SetArmed, intent::SetInputChannels, intent::SetMonitoring,
                                intent::StartSeparation, intent::CancelRun,
                                intent::SetPreRoll, intent::SetCameraArmed, intent::SetCameraDevice,
                                intent::SplitSelectedClip, intent::DeleteSelection, intent::TakeLaneClick, intent::PreviewMix,
                                intent::AddInsert, intent::OpenInsertEditor, intent::ClosePluginWindow,
                                intent::SetExportFormat, intent::SetExportLength, intent::SetExportDestination,
                                intent::SetExportVideoTake, intent::Export, intent::RevealExport,
                                intent::SelectClip, intent::SelectVideoTake, intent::SetTimelineView,
                                intent::SetVerticalScroll, intent::SetTakeLanesUnfolded, intent::SetMixerVisible,
                                intent::SetMixerHeight, intent::SetPassesPanelOpen, intent::ClearClipIndicator>;

using Intent = std::variant<AppIntent, WindowIntent, SongIntent>;

static_assert (core::operationNamesAreUnique<Intent>(), "two Intents share an op name");

// Where an Intent came from. Only the ui.intent_run log line differs: no handler branches on it.
enum class Origin
{
    user,
    control
};

const char* originName (Origin origin);

// What a handler did besides changing state: the id of what it created (add_track,
// split_selected_clip's right half, add_insert's slot), and whether it waits for an answer it
// asked for (camera access) before it acts.
struct IntentDone
{
    std::optional<song::Id> createdId;
    bool awaitingAnswer = false;
};

// An Error is what the handler handled: presented by its tier as the gesture's always was, or, for
// a gesture the UI already shows as unavailable, logged at debug and never presented. Errors raised
// later by work it started (a Pass's save, an export's end) are log events and state, not this.
using IntentResult = core::Expected<IntentDone>;

// Where a Component hands its Intents (MainComponent::run, Origin::user).
using IntentSink = std::function<void (const Intent&)>;

// "show_mode", "move_clip", ...: the op name of the leaf.
std::string_view opName (const Intent& intent);

// A gesture runs it once per frame (a ruler drag, a wheel or pinch, a scroll bar or divider drag, a
// Mixer drag's preview_mix): its struct says `runsPerFrame = true`. MainComponent::run neither logs
// ui.intent_run nor refreshes the window for it; its handler refreshes only what it changes.
bool runsPerFrame (const Intent& intent);

// ui.no_song_open for `op`: what an Intent that needs the open Song answers while none is.
core::Error noSongOpen (std::string_view op);

// The characters macOS delivers for the backspace key (labelled delete) and the forward delete
// key, as JUCE's KeyPress::backspaceKey and KeyPress::deleteKey report them.
inline constexpr char32_t backspaceCharacter = 0x7f;
inline constexpr char32_t forwardDeleteCharacter = 0xf728;
// The characters macOS delivers for the left and right arrow keys (NSLeftArrowFunctionKey,
// NSRightArrowFunctionKey), as JUCE's KeyPress::leftKey and KeyPress::rightKey report them.
inline constexpr char32_t leftArrowCharacter = 0xf702;
inline constexpr char32_t rightArrowCharacter = 0xf703;

// A key press as the window sees it: the character typed (letters in either case) and the
// modifiers held. `command` is the macOS command key.
struct KeyChord
{
    char32_t character = 0;
    bool command = false;
    bool shift = false;
};

// The Intent a key press runs, or none.
std::optional<Intent> intentForKey (KeyChord key);
} // namespace overdub::ui
