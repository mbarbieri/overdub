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
// Keys (issue #9 resolution, issues #27, #28, #128, #200, #225): 1, 2 and 3 pick a mode, R records,
// Space plays or stops, command-Z / shift-command-Z undo and redo, command-E opens the Export sheet,
// I shows or hides the Transcribe column, K adds a Marker at the playhead (issue #227), and S,
// backspace (or forward delete) and the left and right arrows split, delete and nudge, which their
// handlers refuse outside Edit mode (and ui/Availability.h refuses in Transcribe mode). While a
// Marker is selected, backspace and forward delete delete it instead (delete_marker).
namespace overdub::ui
{
enum class Mode
{
    record,
    edit,
    transcribe
};
inline constexpr std::pair<Mode, std::string_view> modeNames[] { { Mode::record, "record" }, { Mode::edit, "edit" }, { Mode::transcribe, "transcribe" } };

// "record", "edit" or "transcribe": the name in ui.* event fields and in the Intent's JSON.
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

struct ReopenLastSong // launch (issue #206): opens the Last Song as open_song_at does
{
    static constexpr std::string_view op = "reopen_last_song";
    bool operator== (const ReopenLastSong&) const = default;
    static constexpr auto fields = std::tuple {};
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

struct ShowMode // keys 1, 2 and 3, the top bar's mode buttons
{
    static constexpr std::string_view op = "show_mode";
    Mode mode = Mode::record;
    bool operator== (const ShowMode&) const = default;
    static constexpr auto fields = std::tuple { enumField ("mode", &ShowMode::mode, core::EnumNames<Mode> { modeNames }) };
};

struct DismissAlerts // OK on every open Alert
{
    static constexpr std::string_view op = "dismiss_alerts";
    bool operator== (const DismissAlerts&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct SetTranscribeColumnVisible // I (issue #225): remembered per Mac (ui/WindowSettings.h), not in the Song
{
    static constexpr std::string_view op = "set_transcribe_column_visible";
    bool visible = true;
    bool operator== (const SetTranscribeColumnVisible&) const = default;
    static constexpr auto fields = std::tuple { field ("visible", &SetTranscribeColumnVisible::visible) };
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

struct MoveMarker // a Marker dragged along the Loop/Marker strip (issue #227)
{
    static constexpr std::string_view op = "move_marker";
    song::Id markerId;
    song::SampleCount position = 0;
    bool operator== (const MoveMarker&) const = default;
    static constexpr auto fields = std::tuple { field ("marker_id", &MoveMarker::markerId), field ("position", &MoveMarker::position) };
};

struct RenameMarker // a name typed after double-clicking a Marker on the strip
{
    static constexpr std::string_view op = "rename_marker";
    song::Id markerId;
    std::string name;
    bool operator== (const RenameMarker&) const = default;
    static constexpr auto fields = std::tuple { field ("marker_id", &RenameMarker::markerId), field ("name", &RenameMarker::name) };
};

struct DeleteMarker // backspace or forward delete while a Marker is selected
{
    static constexpr std::string_view op = "delete_marker";
    song::Id markerId;
    bool operator== (const DeleteMarker&) const = default;
    static constexpr auto fields = std::tuple { field ("marker_id", &DeleteMarker::markerId) };
};

using Edit = std::variant<MoveClip, TrimClipStart, TrimClipEnd, SetClipFadeIn, SetClipFadeOut, SetClipGain, BringBackPass,
                          SetTrackVolume, SetTrackPan, SetMasterVolume, SetTrackMute, SetTrackSolo,
                          RemoveInsert, MoveInsert, SetInsertBypassed, MoveMarker, RenameMarker, DeleteMarker>;

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

struct RecordAgain // ⇧R, File ▸ Record Again, double-Space in Record mode
{
    static constexpr std::string_view op = "record_again";
    bool operator== (const RecordAgain&) const = default;
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
// Song: Transpose (issue #209): one undo step, then renders that playback switches to once ready.

struct SetTranspose // the top bar's Transpose field, on Return
{
    static constexpr std::string_view op = "set_transpose";
    int semitones = 0;
    bool operator== (const SetTranspose&) const = default;
    static constexpr auto fields = std::tuple { field ("semitones", &SetTranspose::semitones) };
};

//==============================================================================
// Song: Practice Speed (issue #212): rehearsal only, never an undo step nor saved; renders that
// playback switches to once ready.

struct SetPracticeSpeed // the top bar's Practice Speed field, on Return
{
    static constexpr std::string_view op = "set_practice_speed";
    int speedPercent = 100;
    bool operator== (const SetPracticeSpeed&) const = default;
    static constexpr auto fields = std::tuple { field ("speed_percent", &SetPracticeSpeed::speedPercent) };
};

//==============================================================================
// Song: the Focus Track and the Transcribe Mix (issue #226, ADR 0010): Transcribe mode's listening
// only, saved in the Song outside history, never an undo step.

struct SetFocusTrack // ◎ on a row of the Transcribe column; none: ◎ on the Focus Track again
{
    static constexpr std::string_view op = "set_focus_track";
    std::optional<song::Id> trackId;
    bool operator== (const SetFocusTrack&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetFocusTrack::trackId) };
};

struct SetTranscribeMute // M on a row of the Transcribe column
{
    static constexpr std::string_view op = "set_transcribe_mute";
    song::Id trackId;
    bool mute = true;
    bool operator== (const SetTranscribeMute&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTranscribeMute::trackId), field ("mute", &SetTranscribeMute::mute) };
};

struct SetTranscribeSolo // S on a row of the Transcribe column
{
    static constexpr std::string_view op = "set_transcribe_solo";
    song::Id trackId;
    bool solo = true;
    bool operator== (const SetTranscribeSolo&) const = default;
    static constexpr auto fields = std::tuple { field ("track_id", &SetTranscribeSolo::trackId), field ("solo", &SetTranscribeSolo::solo) };
};

//==============================================================================
// Song: Markers (issue #227, ADR 0010). Moving, renaming and deleting one are plain edits
// (intent::Edit); Record mode refuses all four (ui/Availability.h).

struct AddMarker // K: "Marker N" at the playhead, one undo step; playback carries on
{
    static constexpr std::string_view op = "add_marker";
    bool operator== (const AddMarker&) const = default;
    static constexpr auto fields = std::tuple {};
};

struct SelectMarker // a click on a Marker (strip or list) seeks to it; none: a click on empty strip
{
    static constexpr std::string_view op = "select_marker";
    std::optional<song::Id> markerId;
    bool operator== (const SelectMarker&) const = default;
    static constexpr auto fields = std::tuple { field ("marker_id", &SelectMarker::markerId) };
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
// Song: Export. The form's choices are stored in the Song, outside history (issue #201); Export
// starts what the form says.

struct ShowExport // Export... in the top bar or the File menu (command-E) opens the sheet; Esc and Cancel close it
{
    static constexpr std::string_view op = "show_export";
    bool shown = true;
    bool operator== (const ShowExport&) const = default;
    static constexpr auto fields = std::tuple { field ("shown", &ShowExport::shown) };
};

struct SetExportFormat // stored in the Song, outside history
{
    static constexpr std::string_view op = "set_export_format";
    exporting::ExportFormat format = exporting::ExportFormat::wav;
    bool operator== (const SetExportFormat&) const = default;
    static constexpr auto fields = std::tuple { enumField ("format", &SetExportFormat::format, core::EnumNames<exporting::ExportFormat> { song::exportFormatNames }) };
};

struct SetExportLength // stored in the Song, outside history
{
    static constexpr std::string_view op = "set_export_length";
    exporting::VideoLength length = exporting::VideoLength::trimToVideoTake;
    bool operator== (const SetExportLength&) const = default;
    static constexpr auto fields = std::tuple { enumField ("length", &SetExportLength::length, core::EnumNames<exporting::VideoLength> { song::videoLengthNames }) };
};

struct SetExportDestination // Choose..., after the save dialog; stored in the Song, outside history; its folder must exist
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

struct CancelExport // the top bar's ✕ beside the export's progress, or File > Cancel Export (issue #202)
{
    static constexpr std::string_view op = "cancel_export";
    bool operator== (const CancelExport&) const = default;
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

struct SetTranscribeView // Transcribe mode's wheel, command-wheel, pinch and overview (issue #225); clamped as TranscribePage::setView clamps
{
    static constexpr std::string_view op = "set_transcribe_view";
    static constexpr bool runsPerFrame = true;
    song::SampleCount firstSample = 0;
    song::SampleCount samplesPerPixel = 0;
    bool operator== (const SetTranscribeView&) const = default;
    static constexpr auto fields = std::tuple { field ("first_sample", &SetTranscribeView::firstSample),
                                                field ("samples_per_pixel", &SetTranscribeView::samplesPerPixel) };
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

using AppIntent = std::variant<intent::NewSongAt, intent::OpenSongAt, intent::ReopenLastSong, intent::DuplicateSongTo, intent::ConvertSampleRate,
                               intent::ShowSettings, intent::ApplyDeviceSettings, intent::MeasureLatency,
                               intent::SaveLatencyCalibration, intent::MeasureClapPass, intent::SaveCameraCalibration,
                               intent::Quit>;

using WindowIntent = std::variant<intent::ShowMode, intent::DismissAlerts, intent::SetTranscribeColumnVisible>;

using SongIntent = std::variant<intent::Edit,
                                intent::Record, intent::RecordAgain, intent::PlayOrStop, intent::Rewind, intent::Undo, intent::Redo,
                                intent::Seek, intent::JumpToPass, intent::Nudge,
                                intent::AddTrack, intent::ImportFiles, intent::DeleteTrack, intent::MoveTrack,
                                intent::SetArmed, intent::SetInputChannels, intent::SetMonitoring,
                                intent::StartSeparation, intent::CancelRun, intent::SetTranspose, intent::SetPracticeSpeed,
                                intent::SetFocusTrack, intent::SetTranscribeMute, intent::SetTranscribeSolo,
                                intent::AddMarker, intent::SelectMarker,
                                intent::SetPreRoll, intent::SetCameraArmed, intent::SetCameraDevice,
                                intent::SplitSelectedClip, intent::DeleteSelection, intent::TakeLaneClick, intent::PreviewMix,
                                intent::AddInsert, intent::OpenInsertEditor, intent::ClosePluginWindow,
                                intent::ShowExport, intent::SetExportFormat, intent::SetExportLength, intent::SetExportDestination,
                                intent::SetExportVideoTake, intent::Export, intent::CancelExport, intent::RevealExport,
                                intent::SelectClip, intent::SelectVideoTake, intent::SetTimelineView, intent::SetTranscribeView,
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

// What the window shows that a key's Intent depends on: I sets the Transcribe column to the
// opposite of what it is, and backspace deletes the selected Marker, when there is one.
struct KeyContext
{
    bool transcribeColumnVisible = true;
    std::optional<song::Id> selectedMarker;
};

// The Intent a key press runs, or none.
std::optional<Intent> intentForKey (KeyChord key, KeyContext context = {});
} // namespace overdub::ui
