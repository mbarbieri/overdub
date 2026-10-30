#pragma once

#include "capture/Camera.h"
#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "export/MixExport.h"
#include "export/MovieExport.h"
#include "plugin/AudioUnitLoader.h"
#include "plugin/InsertHost.h"
#include "song/PracticeSpeed.h"
#include "song/SeparationRun.h"
#include "song/SongPackage.h"
#include "ui/Activity.h"
#include "ui/ExportRun.h"
#include "ui/Listening.h"
#include "video/FrameSelection.h"
#include "video/VideoPlayback.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// One open Song bound to the live engine (issue #27): what the main window's gestures do. It owns
// the package, keeps the engine playing the current Song (reloaded after every change; a change
// only to the mix is applied with Engine::setMix instead, so it plays on without a plan rebuild,
// issue #31), and runs at most one activity at a time: a Pass (capture/PassRecorder.h), a
// Separation, a sample rate conversion or an export (ui/Activity.h).
//
// Refusals (issue #145): ui/Activity.h's refusal() is the one table of which gestures each activity
// refuses. A refused gesture changes nothing and returns that Error (ui.stop_recording_first,
// ui.separation_running, ui.conversion_running or ui.export_running); importFiles returns it for
// each file.
//
// Invariant: every Pass it starts is finished by it. stop() asks the audio thread to
// end the Pass; poll() (from the window's timer) ends its movie once the audio thread has ended
// it, and finishes it once the camera has completed the movie, without waiting for it (issue #70:
// AVFoundation may take seconds). Until then isRecording() stays true (isSavingPass() tells a stopped Pass from a capturing one) and the Song stays as the
// Pass started. Detaching the audio waits for the audio thread only (issue #90): a Pass whose movie
// is still completing stays pending and poll() finishes it, with no engine. Destroying the
// OpenSong waits for the movie and finishes the Pass.
//
// While a Pass runs (pre-roll included), only stop, play/stop and rewind act: undo, redo, edits,
// seeks, import, arm, input, pre-roll, Monitoring, the camera, Mixer previews, export, Separation
// and conversion are refused, so the Song and the engine's plan stay as the Pass started.
//
// The camera (issue #35): the Song's camera arm and camera choice live outside history. While the
// camera is Armed it is open (its live feed shows on the Record stage) and each Pass also records a
// Video Take, placed with this machine's calibration for that camera and the output device
// (capture/CameraCalibration.h). Closing the Song closes the camera.
//
// Video Take playback (issue #36): playbackClock() is what the engine says about the transport now
// (ADR 0003's pull model); videoPlayback() reads the Song's movies and decodes the frame shown.
//
// Insert Chains (issue #33): it hosts the Song's plugins in a plugin::InsertHost, created through
// the Instantiator it is given (out-of-process AUs in the app). The engine plays each plugin once
// it has loaded; a plugin that loads or changes its latency rebuilds the plan at the next poll()
// (after the Pass, while one runs). The package captures every plugin's state into its slot on
// each history step and debounced write; a parameter change schedules that write. Undo keeps a
// surviving slot's plugin as it is and loads a slot it brings back with the snapshot's state.
//
// Export (issue #37): startExport stops the transport and hands the engine an empty Song, so the
// plan that runs the Song's plugins stops rendering; an ExportRun (ui/ExportRun.h) then owns the
// export's custody of the plugins: poll() tells it whether the audio thread has taken the empty
// plan (Engine::hasPendingPlan), and it renders the Song through its own plan of the same graph,
// sharing the plugins, or gives the export up after ExportRun::audioTimeout (issue #64). While it
// runs, record, play, import, Track delete, Separation and conversion are refused
// (ui.export_running), and edits change the Song but not the engine; the InsertHost follows them through the run (a removed slot stays hosted
// until the export ends, issue #65). When it ends, poll() reloads the engine with the current Song and
// returns its Error. Without audio the export starts at once. Detaching the audio stops a running
// export (logged as ui.export_stopped).
//
// Plugin crash recovery (issue #40, ADR 0002 amendment): when a plugin's hosting process dies, the
// InsertHost marks its slot crashed and its nodes pass audio through at once. poll() reports it
// (ui.plugin_crashed, or ui.plugin_crashed_repeatedly once it will not reload: Notices), rebuilds the
// plan without it (after the Pass, while one runs: recording goes on, Takes are dry anyway), and
// reloads it from its slot's saved state once no Pass or export runs. The ExportRun hears each
// crash first: one in a plugin the export renders discards it (ui.export_plugin_crashed, an Alert,
// before the Notice).
//
// Device changes (issue #41, ADR 0004 amendment): the Song's rate is fixed, so a device at another
// rate is not used (audioProblem() says ui.sample_rate_mismatch; play and record do nothing). A
// device that stops during a Pass ends it where it stopped (Engine::deviceStopped): the Pass is
// finished as usual, keeping what was captured, and reported as ui.audio_device_lost_during_pass
// (an Alert). poll() also lists the cameras again when one is plugged in or out; the Armed camera
// unplugged is closed and disarmed (ui.camera_disconnected, a Notice) at once, or after the Pass
// when one runs: recording goes on and the Pass keeps the movie written until then.
//
// Separation (issue #114, ADR 0007): startSeparation stops the transport and starts a
// song::SeparationRun on a Track's Backing Track with the tools set by setSeparationTools; poll()
// advances it, and once it ends reloads the engine (the six new Tracks, the source Track muted) and
// returns its Error. While it runs, record, play, edits, undo, redo, import, arm, input, pre-roll,
// Monitoring, the camera, Mixer previews, export and conversion are refused (ui.separation_running):
// the Separation dialog is modal, and the Song stays as the Separation started. It is refused while
// a Pass (ui.stop_recording_first), a conversion (ui.conversion_running) or an export
// (ui.export_running) runs. cancelSeparation() (the dialog's
// Cancel) and closing the Song kill the helper; the Song is unchanged.
//
// Sample rate conversion (issue #160, ADR 0004 amendment): startSampleRateConversion stops the
// transport and starts a song::SampleRateConversionRun; poll() advances it and returns its Error.
// While it runs the Song is locked as during a Separation (refused with ui.conversion_running,
// export included), and it is refused while a Pass, an export or a Separation runs. When the
// Song's rate changes (the conversion, or an undo or redo across one), the audio is detached at once
// (nothing plays the Song at the wrong speed) and takeSampleRateChanged() tells the owner to open
// the device at the new rate (logged as ui.song_rate_changed: from_rate, to_rate).
//
// Transpose, Practice Speed and the octave shift (issues #209, #212, #231, #233; ADR 0009, ADR
// 0010) play offline renders that a ui::Listening (ui/Listening.h) makes and switches to: poll()
// advances it and hands back a failed render's Error once (a Notice), every change of the Song or
// of what is wanted follows it at once, and the engine reloads whenever what is heard changes. The
// engine plays heard().song at the heard Practice Speed. While
// isTransposing(), Record and Export are unavailable (ui/Availability.h); after a failed render they
// stay so until the Transpose changes, since a Pass or an export would not match what is heard.
//
// The Practice Speed is held by Listening, outside the Song and its history; a Song always opens at
// 100% (a Loop heard in Transcribe mode then brings back its own, issue #230). While a Loop is heard
// (heard().loop), its window renders first.
//
// The octave shift (issue #233): each Track keeps its own (song::Track::octaveShift, outside
// history), and only the Focus Track's is heard, only while transcribing (heard().octaveShiftedTrack).
// It is heard at once: a source with no render yet plays silence until its run ends (the spinner).
//
// The band filter (issue #234, ADR 0010): each Track keeps its own (song::Track::bandFilter, outside
// history), and only the Focus Track's is heard, only while transcribing and while it is on
// (heard().bandFilter). It runs live in the engine (Engine::setBandFilter, set with the Loop), so a
// change is heard from the next block, with no render or rebuild.
//
// Message thread only.
namespace overdub::ui
{
// The audio an OpenSong plays and records through. A null engine means no device is open.
struct AudioBinding
{
    engine::Engine* engine = nullptr;
    capture::RecordLatency latency;
    // The device's active input channel names, in the order process() receives them.
    std::vector<std::string> inputChannelNames;
    // The output device's name: with the camera's id, the key of this machine's camera calibration.
    std::string outputDeviceName = {};
    // The device's sample rate in Hz; 0 when it reports none (taken to be the Song's). A device at
    // another rate than the Song's is not used (issue #41, ADR 0004 amendment).
    int sampleRate = 0;
    // The rates the device offers, in Hz: whether the mismatch can be fixed in Settings.
    std::vector<int> offeredSampleRates = {};
};

class OpenSong
{
public:
    // Loads the Song into the engine, with Monitoring on for no Track, at the Song's playhead, and
    // starts loading its plugins through `instantiate` (none load when it is empty). Its activities
    // are counted in `sharedActivityLog` (the Workspace's, which must outlive it), or in its own
    // when null.
    OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio, plugin::Instantiator instantiate = {}, ActivityLog* sharedActivityLog = nullptr);
    // detachAudio() (errors logged), then the package captures the plugins' state and writes what
    // is pending as it closes.
    ~OpenSong();

    OpenSong (const OpenSong&) = delete;
    OpenSong& operator= (const OpenSong&) = delete;

    const song::Song& song() const { return package_->song(); }
    song::SongPackage& package() { return *package_; }
    const song::SongPackage& package() const { return *package_; }
    const AudioBinding& audio() const { return binding; }

    // Binds a (new) engine: loads the Song and seeks to its playhead. Call detachAudio() first, and
    // only once the previously bound engine has stopped rendering (its device closed or the engine
    // destroyed, as MainWindow does): the plugins are re-prepared here for the new device shape.
    // A device at another rate than the Song's is not bound (see audioProblem()).
    void attachAudio (AudioBinding audio);
    // Why the Song has no audio: ui.no_audio_device (no device is open), or ui.sample_rate_mismatch
    // (song_rate, device_rate: the device runs at another rate, so it is not used). None when the
    // engine is bound. Not logged here.
    std::optional<core::Error> audioProblem() const;
    // Finishes a running Pass (waiting for the audio thread), stops the transport and leaves the
    // engine an empty Song. Returns the finishing errors, not logged.
    std::vector<core::Error> detachAudio();

    // A new loopback calibration for the bound device (issue #39): the next Pass is placed with it.
    // The engine and its plan are untouched. A running Pass keeps the latency it started with.
    void setCalibrationOffset (song::SampleCount offsetSamples) { binding.latency.calibrationOffsetSamples = offsetSamples; }

    ActivityKind activityKind() const { return kindOf (activity); }
    const ActivityLog& activities() const { return activityLog; }

    // True from record() until poll() (or detachAudio(), when the movie is complete) has finished the Pass (its movie completed).
    bool isRecording() const { return std::holds_alternative<PassActivity> (activity); }
    // True while isRecording() but the audio thread has ended the Pass: nothing is captured any more,
    // the Pass is only being saved (its movie completing, issue #91). Gestures stay refused.
    bool isSavingPass() const;
    // The number of the Pass being recorded (from 1); 0 when not recording.
    int recordingPassNumber() const;
    bool isPlaying() const;
    song::SampleCount position() const;
    // The transport as the video sees it: playing, position, the latest anchor and the device's
    // output latency (stopped at the Song's playhead without audio).
    video::PlaybackClock playbackClock() const;
    // The Song's Video Take playback (movie timings, the frame decoder).
    video::VideoPlayback& videoPlayback() { return playback; }

    // Starts a Pass from the playhead (and the transport, if stopped). Does nothing while a Pass
    // runs. With the transport stopped and the Song's pre-roll on (issue #32), playback starts that
    // many seconds before the playhead (at 0 at the earliest) and the Pass waits for the playhead,
    // its record point: only from there on is input captured.
    // Errors: refusal()'s, audioProblem()'s, or PassRecorder::start's (capture.nothing_armed, ...).
    core::Expected<void> record();
    // Record Again (issue #216): a new Pass, as record() starts one, at the record point of the
    // Song's most recent Pass, with the transport stopped there first so pre-roll applies. During a
    // Pass, stops it and starts the new one at its record point once poll() has finished it.
    // Errors: record()'s. The Song has a Pass, or one runs (ui/Availability.h refuses it otherwise).
    core::Expected<void> recordAgain();
    // The record point while a Pass waits for it during pre-roll; none otherwise.
    std::optional<song::SampleCount> preRollRecordPoint() const;
    // The running Pass's record point once its capture has begun; none otherwise.
    std::optional<song::SampleCount> runningPassStart() const;
    // Stops the transport and asks the audio thread to end a running Pass.
    void stop();
    // Plays, or stops (as stop()) when playing or recording. Does nothing without audio. While a
    // Loop is heard (heard().loop), playing starts at its start unless the playhead is inside it.
    // Errors: refusal()'s.
    core::Expected<void> playOrStop();
    // To the start of the Song; ends a running Pass (as stop()).
    void rewind();
    // Moves the playhead to this Song sample (0 or later). Errors: refusal()'s.
    core::Expected<void> seek (song::SampleCount sample);

    // Errors: refusal()'s, SongPackage::undo / redo's (song.history_empty when there is no step).
    core::Expected<void> undo();
    core::Expected<void> redo();

    // An undoable change (Edit mode's Clip operations): one history step named `label`, then the
    // engine plays the changed Song.
    // Errors: refusal()'s, SongPackage::edit's.
    core::Expected<void> edit (song::Song changed, std::string label);

    // The engine plays this Song's mix (a Mixer gesture in progress) without changing the Song or
    // its history; does nothing without audio or unless it differs from the Song only in its mix.
    // A finished gesture is an edit(); an abandoned one previews song() again.
    // Errors: refusal()'s.
    core::Expected<void> previewMix (const song::Song& candidate);
    // Each Mixer strip's peak since the previous call (Engine::takeMeterPeaks); none without audio.
    engine::MeterPeaks meterPeaks();

    // The peaks of a Clip's source from the package's cache (rebuilt there when missing), kept
    // here once read. When they cannot be had, the Error is logged once (warn) and the result has
    // no channels: the Clip is drawn without a waveform.
    const song::Peaks& peaks (const song::ClipSource& source);

    // Adds this AU at the end of the Track's Insert Chain (the Master's for an empty trackId) as
    // one undo step "Add AUDelay to Guitar"; it is heard once it has loaded. Returns the new
    // slot's id.
    // Errors: refusal()'s, song.track_not_found, SongPackage::edit's.
    core::Expected<song::Id> addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin);
    // A plugin's state may have changed without a parameter report (its editor window closed):
    // schedules the debounced song.json write, which captures every plugin's state. No undo step.
    void notePluginStateChanged();
    // Whether the slot's plugin is loading, ready or failed; none for a slot not in the Song (but
    // one removed during an export, hosted until it ends).
    std::optional<plugin::InsertStatus> insertStatus (const song::Id& slotId) const { return inserts.status (slotId); }
    plugin::InsertHost& insertHost() { return inserts; }

    // Starts exporting the mix (with the request's Video Take for an MP4, issue #38) to the
    // request's file (see the header comment); `now` starts ExportRun::audioTimeout. Does nothing
    // while an export runs.
    // Errors: refusal()'s, ui.no_video_take (an MP4 without a Video Take of the Song),
    // export.folder_missing (the destination's folder does not exist; it is not created), and
    // without audio ui.export_plugin_crashed (ExportRun's).
    core::Expected<void> startExport (exporting::ExportRequest request, song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());
    // Cancels the running export (issue #202, ExportRun::cancel): its partial file goes, a file
    // that was at the destination stays, and the engine plays the Song again. Returns how it
    // ended: ui.export_cancelled, the render's own Error when it failed before it heard the stop,
    // or none when it had written its file already (kept, as lastExportedFile) or none ran.
    std::optional<core::Error> cancelExport();
    ExportPhase exportPhase() const;
    bool isExporting() const { return std::holds_alternative<ExportActivity> (activity); }
    // The fraction of the running export written, 0..1 (0 while it waits for the audio).
    double exportProgress() const;
    // The file the last export wrote, once it finished; none before (or after one failed).
    const std::optional<juce::File>& lastExportedFile() const { return lastExport; }

    // Stores a change of the Song's settings outside history (song.json, no undo step), such as
    // the Export sheet's choices (ui/ExportSettings.h). Nothing when the Song is unchanged; the
    // change's Error, changing nothing. Refuses nothing: no activity locks these settings.
    core::Expected<void> changeSettings (core::Expected<song::Song> changed);

    // Transcribe mode (issue #226, ADR 0010). While transcribing, the engine plays the Transcribe
    // Mix's mute and solo instead of the Song's (song::withTranscribeMix); otherwise the Song's own
    // mix. The window sets it as its mode changes; it is not in the Song. Changing it applies the mix
    // (Engine::setMix) without rebuilding the plan unless the octave-shifted Track changes.
    void setTranscribing (bool transcribing);
    // The Focus Track and the Transcribe Mix: Song settings outside history (song.json, no undo
    // step), song/TranscribeMix.h, heard at once while transcribing. Log ui.focus_track_set (info:
    // track_id, "" for none), ui.transcribe_mute_set and ui.transcribe_solo_set (debug: track_id,
    // mute / solo). Errors: refusal()'s, song.track_not_found.
    core::Expected<void> setFocusTrack (const std::optional<song::Id>& trackId);
    core::Expected<void> setTranscribeMute (const song::Id& trackId, bool mute);
    core::Expected<void> setTranscribeSolo (const song::Id& trackId, bool solo);
    // A Track's octave shift, -1..+2 (see the header comment): a Song setting outside history.
    // Logs ui.octave_shift_set (info: track_id, octaves). Errors: refusal()'s,
    // song.octave_shift_out_of_range, song.track_not_found.
    core::Expected<void> setOctaveShift (const song::Id& trackId, int octaves);
    // A Track's band filter (issue #234, see the header comment): a Song setting outside history.
    // Logs ui.band_filter_set (info: track_id, on, low_hz, high_hz). Errors: refusal()'s,
    // song.band_filter_out_of_range, song.band_filter_low_not_below_high, song.track_not_found.
    core::Expected<void> setBandFilter (const song::Id& trackId, song::BandFilter filter);

    // Loops (issue #228, ADR 0010). The active Loop and looping are Song settings outside history
    // (song.json, no undo step, song/LoopEdits.h). The engine loops (Engine::setLoop) only while
    // transcribing, looping and with an active Loop: heard().loop. Logs ui.active_loop_set (info:
    // loop_id, "" for none, looping) and ui.looping_set (info: looping). Each Loop keeps its Practice
    // Speed (issue #230, ui/Listening.h), except while refusal() refuses set_practice_speed.
    // Errors: refusal()'s, song.loop_not_found; setLooping: song.no_active_loop.
    core::Expected<void> setActiveLoop (const std::optional<song::Id>& loopId, bool looping);
    core::Expected<void> setLooping (bool looping);

    // Pre-roll is a Song setting outside history: written to song.json, never an undo step.
    // Errors: refusal()'s.
    core::Expected<void> setPreRoll (song::PreRoll preRoll);

    // Errors: refusal()'s.
    core::Expected<void> setArmed (const song::Id& trackId, bool armed);
    core::Expected<void> setInputChannels (const song::Id& trackId, std::vector<int> inputChannels);
    // The camera Video Takes are recorded from (not owned; it must outlive this OpenSong, or be
    // detached with null first) and the camera calibration file. Opens the camera when the Song's
    // camera is Armed. Errors: the camera's open() Errors (the camera is then disarmed).
    core::Expected<void> attachCamera (capture::Camera* camera, juce::File calibrationFile);
    // Arms (opening the Song's chosen camera, or the default one, which then becomes the choice) or
    // disarms (closing it) the camera. Without a camera, arming does nothing.
    // Errors: refusal()'s, the camera's open() Errors (the camera stays disarmed).
    core::Expected<void> setCameraArmed (bool armed);
    // Chooses the camera (reopened when Armed).
    // Errors: refusal()'s, the camera's open() Errors (the camera is then disarmed).
    core::Expected<void> setCameraDevice (const std::string& cameraId);
    // The cameras on offer, as listed when the camera was attached, armed or chosen.
    const std::vector<capture::CameraDevice>& cameraDevices() const { return cameras; }
    // The live feed (a CALayer*) while the camera is Armed and open; null otherwise.
    void* cameraPreviewLayer() const;

    // Monitoring is transient (not in the Song): hearing an Armed Track's live input. The engine is
    // given the monitored Tracks with the Song. Errors: refusal()'s.
    core::Expected<void> setMonitoring (const song::Id& trackId, bool on);
    bool isMonitoring (const song::Id& trackId) const { return monitored.contains (trackId); }
    const std::set<song::Id>& monitoredTracks() const { return monitored; }

    // One Backing Track import per file (SongPackage::importBackingTracks); refusal()'s for each
    // file while the activity refuses Gesture::changeTracks.
    std::vector<core::Expected<song::Id>> importFiles (const std::vector<juce::File>& files);

    // The model and helper a Separation uses (none by default: Separation is unavailable).
    void setSeparationTools (song::SeparationTools tools) { separationTools = std::move (tools); }
    // Starts separating the Track's Backing Track (see the header comment). Does nothing while one
    // runs. Errors: refusal()'s, song::SeparationRun::start's.
    core::Expected<void> startSeparation (const song::Id& trackId, song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());
    bool isSeparating() const { return std::holds_alternative<SeparationActivity> (activity); }
    // Cancels the running Separation (song::SeparationRun::cancel: the helper is killed, its
    // output deleted, the Song unchanged) and reloads the engine. Nothing when none runs.
    void cancelSeparation (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());
    // The running Separation's progress and source file name; empty when none runs.
    song::SeparationProgress separationProgress() const;
    std::string separationSourceName() const;

    // Starts converting the Song to `toRate` (see the header comment). Does nothing while one runs.
    // Errors: refusal()'s, song::SampleRateConversionRun::start's.
    core::Expected<void> startSampleRateConversion (int toRate, song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());
    bool isConverting() const { return std::holds_alternative<ConversionActivity> (activity); }
    // Stops the running conversion (its files deleted, the Song unchanged). Nothing when none runs.
    void cancelSampleRateConversion (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());
    // The running conversion's fraction done (0..1) and target rate; 0 when none runs.
    double conversionFraction() const;
    int conversionTargetRate() const;
    // True once after the Song's rate changed: the audio is detached and should be opened again at
    // the Song's rate.
    bool takeSampleRateChanged() { return std::exchange (sampleRateChanged, false); }

    // What playback hears of the Song (ui/Listening.h): the heard Song, Loop, band filter and
    // octave-shifted Track.
    Heard heard() const { return listening_.heard (song()); }
    // Transcribing, the Practice Speed and the renders playback uses (see the header comment).
    const Listening& listening() const { return listening_; }
    // Playback is not at the Song's Transpose yet, or renders for it are being made.
    bool isTransposing() const { return listening_.isTransposing (song()); }

    // Sets the Practice Speed (see the header comment). The same value again tries its failed
    // renders again. While a Loop is heard it is also that Loop's Practice Speed, outside history
    // (issue #230). Errors: refusal()'s, song.practice_speed_out_of_range.
    core::Expected<void> setPracticeSpeed (int speedPercent);

    // Call regularly (window timer): advances the Listening Renders (see the header comment), an export (starts it once the audio has let go of
    // the plugins, or gives it up after ExportRun::audioTimeout; once it ends, reloads the engine and
    // returns its Error), finishes a Pass the audio thread has ended, rebuilds the plan when a
    // plugin loaded or changed its latency (not during a Pass), schedules a write when a plugin
    // parameter changed, saves the playhead once the transport is stopped, writes a due song.json,
    // and handles plugin crashes (see the header comment). Returns the errors, not logged.
    std::vector<core::Error> poll (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

private:
    // Syncs the InsertHost with the Song and loads it into the engine. While an export runs, the
    // ExportRun follows the Song instead and the engine keeps its empty Song; after it, endExport reloads.
    void reloadEngine();
    // Whether the audio thread no longer renders a plan that holds the plugins (none without audio).
    bool audioLetGoOfPlugins() const;
    // When the ExportRun says it ended: ends the export, reloads the engine and keeps its file.
    // Returns its Error; none (nothing done) while it runs.
    std::optional<core::Error> endExportIfEnded (std::optional<ExportEnd> end);
    // Advances a running Separation; once it ends, reloads the engine and returns its Error.
    std::optional<core::Error> advanceSeparation (song::SongPackage::Clock::time_point now);
    // Stores a Transcribe Mix, Focus Track, octave shift or band filter change outside history and
    // plays it (hearChange). Nothing when the Song is unchanged.
    core::Expected<void> changeListening (core::Expected<song::Song> changed);
    // Stores an active Loop or looping change outside history, plays at the heard Loop's Practice
    // Speed when it may change, and plays it (hearChange), also when the Song is unchanged.
    core::Expected<void> changeHeardLoop (core::Expected<song::Song> changed);
    // After the Song or the listening state changed from `before`: the engine plays heard() (with
    // Engine::setMix when only the heard mix changed and the octave-shifted Track did not, else
    // reloadEngine), and the renders follow.
    void hearChange (const Heard& before);
    // refusal() does not refuse set_practice_speed now.
    bool practiceSpeedMayChange() const;
    // Advances a running conversion; once it ends, updates the engine and returns its Error.
    std::optional<core::Error> advanceConversion (song::SongPackage::Clock::time_point now);
    // After a history change: detaches the audio when the rate changed since `before`, else
    // hearChange.
    void updateEngine (const Heard& before);
    // Every change of `activity` goes through these two, which count it in the ActivityLog.
    void beginActivity (Activity started);
    void endActivity (std::optional<std::string> errorCode);
    // record() with its record point, and whether the transport plays on through it.
    core::Expected<void> startPass (song::SampleCount recordPoint, bool playing);
    // Stops the transport at `recordPoint` and starts a Pass there, logging ui.record_again with the
    // Song's most recent Pass, which it follows. The Song has a Pass.
    core::Expected<void> recordAgainFrom (song::SampleCount recordPoint);
    // Ends the running Pass (one must run) with what its recorder finished.
    std::vector<core::Error> finishPass (capture::FinishedPass finished);
    // The crashes the InsertHost heard since the last poll, as Notice errors (ui.plugin_crashed,
    // ui.plugin_crashed_repeatedly: slot_ids, names), after the export's Error when they end it.
    std::vector<core::Error> handleCrashes();
    // Lists the cameras again once the camera reports a change; the Armed camera gone is
    // ui.camera_disconnected (camera_id, during_pass) and is disarmed now, or after the Pass.
    std::vector<core::Error> handleCameraChanges();
    void disarmLostCamera();
    void capturePluginState (song::Song& song);
    // Opens the Song's chosen camera when it is Armed, closes it otherwise. On failure the Song's
    // camera is disarmed and the Error returned.
    core::Expected<void> syncCamera();
    capture::SampleCount cameraCalibrationSamples (const std::string& cameraId) const;
    static int blockSizeOf (const AudioBinding& audio);

    ActivityLog ownActivityLog;
    ActivityLog& activityLog;
    std::uint64_t activityCounter = 0;
    std::unique_ptr<song::SongPackage> package_;
    song::SeparationTools separationTools;
    bool sampleRateChanged = false;
    // After package_: destroyed first, once the destructor has captured its state and unhooked it.
    plugin::InsertHost inserts;
    AudioBinding binding;
    std::set<song::Id> monitored;
    std::map<song::Id, song::Peaks> peaksByMediaId; // media is immutable, so entries never go stale
    capture::Camera* camera = nullptr;
    juce::File cameraCalibrationFile;
    std::vector<capture::CameraDevice> cameras;
    // Set while a device at another rate than the Song's is attached (ui.sample_rate_mismatch).
    std::optional<core::Error> rateMismatch;
    // Record Again ran during a Pass: poll() starts the next Pass once that one is finished.
    bool recordAgainAfterPass = false;
    video::VideoPlayback playback;
    std::optional<juce::File> lastExport;
    // Before `activity`: a run reads only its own files, never the package.
    Listening listening_;
    // Last: destroyed first, so a running export stops rendering before the plugins and the package
    // go, and a running Separation (its helper killed, its output folder deleted) or conversion (its
    // files deleted) is cancelled before the package.
    Activity activity;
};
} // namespace overdub::ui
