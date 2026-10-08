#pragma once

#include "capture/Camera.h"
#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "export/MixExport.h"
#include "export/MovieExport.h"
#include "plugin/AudioUnitLoader.h"
#include "plugin/InsertHost.h"
#include "song/SongPackage.h"
#include "video/FrameSelection.h"
#include "video/VideoPlayback.h"

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// One open Song bound to the live engine (issue #27): what the main window's gestures do. It owns
// the package, keeps the engine playing the current Song (reloaded after every change; a change
// only to the mix is applied with Engine::setMix instead, so it plays on without a plan rebuild,
// issue #31), and runs at most one Pass (capture/PassRecorder.h).
//
// Invariant: every Pass it starts is finished by it. stop() asks the audio thread to
// end the Pass; poll() (from the window's timer) finishes it once the audio thread has; detaching
// the audio or destroying it waits for the audio thread and finishes it there.
//
// While a Pass runs (pre-roll included), only stop, play/stop and rewind act: undo, redo, edits,
// seeks, import, arm, input, pre-roll and Monitoring are ignored, so the Song and the engine's plan stay as the Pass started.
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
// plan that runs the Song's plugins stops rendering; once the audio thread has taken the empty
// plan (Engine::hasPendingPlan, checked by poll()), the export renders the Song through its own
// plan of the same graph, sharing the plugins, on a worker thread (exporting::ExportJob). While it
// runs, record and play are refused (ui.export_running), and edits change the Song but not the
// engine; when it ends, poll() reloads the engine with the current Song. Without audio the export
// starts at once. Detaching the audio stops a running export (logged as ui.export_stopped).
// Known limit: an engine whose device has stopped calling back without being detached never takes
// the empty plan, so the export waits ("Preparing…") until the device runs or is detached. A slot
// an edit removes during an export keeps rendering in it (the plan holds the plugin) until it ends.
//
// Message thread only.
namespace overdub::ui
{
enum class ExportPhase
{
    idle,
    waitingForAudio, // the audio thread still renders the Song's plan
    rendering
};

// The audio an OpenSong plays and records through. A null engine means no device is open.
struct AudioBinding
{
    engine::Engine* engine = nullptr;
    capture::RecordLatency latency;
    // The device's active input channel names, in the order process() receives them.
    std::vector<std::string> inputChannelNames;
    // The output device's name: with the camera's id, the key of this machine's camera calibration.
    std::string outputDeviceName = {};
};

class OpenSong
{
public:
    // Loads the Song into the engine, with Monitoring on for no Track, at the Song's playhead, and
    // starts loading its plugins through `instantiate` (none load when it is empty).
    OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio, plugin::Instantiator instantiate = {});
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
    void attachAudio (AudioBinding audio);
    // Finishes a running Pass (waiting for the audio thread), stops the transport and leaves the
    // engine an empty Song. Returns the finishing errors, not logged.
    std::vector<core::Error> detachAudio();

    // A new loopback calibration for the bound device (issue #39): the next Pass is placed with it.
    // The engine and its plan are untouched. A running Pass keeps the latency it started with.
    void setCalibrationOffset (song::SampleCount offsetSamples) { binding.latency.calibrationOffsetSamples = offsetSamples; }

    // True from record() until poll() or detachAudio() has finished the Pass.
    bool isRecording() const { return recorder != nullptr; }
    // The number of the Pass being recorded (from 1); 0 when not recording.
    int recordingPassNumber() const { return recordingPass; }
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
    // Errors: ui.no_audio_device, or PassRecorder::start's (capture.nothing_armed, ...).
    core::Expected<void> record();
    // The record point while a Pass waits for it during pre-roll; none otherwise.
    std::optional<song::SampleCount> preRollRecordPoint() const;
    // The running Pass's record point once its capture has begun; none otherwise.
    std::optional<song::SampleCount> runningPassStart() const;
    // Stops the transport and asks the audio thread to end a running Pass.
    void stop();
    // Plays, or stops (as stop()) when playing or recording.
    void playOrStop();
    // To the start of the Song; ends a running Pass (as stop()).
    void rewind();
    // Moves the playhead to this Song sample (0 or later). Ignored while a Pass runs.
    void seek (song::SampleCount sample);

    // Errors: SongPackage::undo / redo's (song.history_empty when there is no step).
    core::Expected<void> undo();
    core::Expected<void> redo();

    // An undoable change (Edit mode's Clip operations): one history step named `label`, then the
    // engine plays the changed Song. Ignored (returns success) while a Pass runs.
    // Errors: SongPackage::edit's.
    core::Expected<void> edit (song::Song changed, std::string label);

    // The engine plays this Song's mix (a Mixer gesture in progress) without changing the Song or
    // its history; ignored unless it differs from the Song only in its mix, and while a Pass runs.
    // A finished gesture is an edit(); an abandoned one previews song() again.
    void previewMix (const song::Song& candidate);
    // Each Mixer strip's peak since the previous call (Engine::takeMeterPeaks); none without audio.
    engine::MeterPeaks meterPeaks();

    // The peaks of a Clip's source from the package's cache (rebuilt there when missing), kept
    // here once read. When they cannot be had, the Error is logged once (warn) and the result has
    // no channels: the Clip is drawn without a waveform.
    const song::Peaks& peaks (const song::ClipSource& source);

    // Adds this AU at the end of the Track's Insert Chain (the Master's for an empty trackId) as
    // one undo step "Add AUDelay to Guitar"; it is heard once it has loaded. Returns the new
    // slot's id; an empty id while a Pass runs (nothing is added).
    // Errors: song.track_not_found, SongPackage::edit's.
    core::Expected<song::Id> addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin);
    // A plugin's state may have changed without a parameter report (its editor window closed):
    // schedules the debounced song.json write, which captures every plugin's state. No undo step.
    void notePluginStateChanged();
    // Whether the slot's plugin is loading, ready or failed; none for a slot not in the Song.
    std::optional<plugin::InsertStatus> insertStatus (const song::Id& slotId) const { return inserts.status (slotId); }
    plugin::InsertHost& insertHost() { return inserts; }

    // Starts exporting the mix (with the request's Video Take for an MP4, issue #38) to the
    // request's file (see the header comment). Ignored while an export runs.
    // Errors: ui.stop_recording_first while a Pass runs, ui.no_video_take (an MP4 without a
    // Video Take of the Song).
    core::Expected<void> startExport (exporting::ExportRequest request);
    ExportPhase exportPhase() const;
    bool isExporting() const { return exportPhase() != ExportPhase::idle; }
    // The fraction of the running export written, 0..1 (0 while it waits for the audio).
    double exportProgress() const;
    // The file the last export wrote, once it finished; none before (or after one failed).
    const std::optional<juce::File>& lastExportedFile() const { return lastExport; }

    // The Video Take an MP4 export shows: a Song setting outside history (song.json, no undo
    // step), the Export page's default. Ignored for an id that is not a Video Take of the Song.
    void setExportVideoTake (const song::Id& videoTakeId);

    // Pre-roll is a Song setting outside history: written to song.json, never an undo step.
    // Ignored while a Pass runs.
    void setPreRoll (song::PreRoll preRoll);

    void setArmed (const song::Id& trackId, bool armed);
    void setInputChannels (const song::Id& trackId, std::vector<int> inputChannels);
    // The camera Video Takes are recorded from (not owned; it must outlive this OpenSong, or be
    // detached with null first) and the camera calibration file. Opens the camera when the Song's
    // camera is Armed. Errors: the camera's open() Errors (the camera is then disarmed).
    core::Expected<void> attachCamera (capture::Camera* camera, juce::File calibrationFile);
    // Arms (opening the Song's chosen camera, or the default one, which then becomes the choice) or
    // disarms (closing it) the camera. Ignored while a Pass runs. Without a camera, arming does
    // nothing. Errors: the camera's open() Errors (the camera stays disarmed).
    core::Expected<void> setCameraArmed (bool armed);
    // Chooses the camera (reopened when Armed). Ignored while a Pass runs.
    // Errors: the camera's open() Errors (the camera is then disarmed).
    core::Expected<void> setCameraDevice (const std::string& cameraId);
    // The cameras on offer, as listed when the camera was attached, armed or chosen.
    const std::vector<capture::CameraDevice>& cameraDevices() const { return cameras; }
    // The live feed (a CALayer*) while the camera is Armed and open; null otherwise.
    void* cameraPreviewLayer() const;

    // Monitoring is transient (not in the Song): hearing an Armed Track's live input.
    void setMonitoring (const song::Id& trackId, bool on);
    bool isMonitoring (const song::Id& trackId) const { return monitored.contains (trackId); }

    // One Backing Track import per file (SongPackage::importBackingTracks); empty during a Pass.
    std::vector<core::Expected<song::Id>> importFiles (const std::vector<juce::File>& files);

    // Call regularly (window timer): advances an export (starts it once the audio has let go of
    // the plugins; once it ends, reloads the engine and returns its Error), finishes a Pass the
    // audio thread has ended, rebuilds the plan
    // when a plugin loaded or changed its latency (not during a Pass), schedules a write when a
    // plugin parameter changed, saves the playhead once the transport is stopped, and writes a due
    // song.json. Returns the errors, not logged.
    std::vector<core::Error> poll (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

private:
    // Does nothing while an export runs (the engine keeps its empty Song); after it, poll() reloads.
    void reloadEngine();
    void startExportOnceAudioLetGo();
    std::vector<core::Error> advanceExport();
    void stopExport (const char* reason);
    // After a history change: setMix when only the mix changed since `before`, else reloadEngine.
    void updateEngine (const song::Song& before);
    std::vector<core::Error> finishPass (core::Expected<capture::RecordedPass> finished);
    void capturePluginState (song::Song& song);
    // Opens the Song's chosen camera when it is Armed, closes it otherwise. On failure the Song's
    // camera is disarmed and the Error returned.
    core::Expected<void> syncCamera();
    capture::SampleCount cameraCalibrationSamples (const std::string& cameraId) const;
    static int blockSizeOf (const AudioBinding& audio);

    std::unique_ptr<song::SongPackage> package_;
    // After package_: destroyed first, once the destructor has captured its state and unhooked it.
    plugin::InsertHost inserts;
    AudioBinding binding;
    std::set<song::Id> monitored;
    std::map<song::Id, song::Peaks> peaksByMediaId; // media is immutable, so entries never go stale
    capture::Camera* camera = nullptr;
    juce::File cameraCalibrationFile;
    std::vector<capture::CameraDevice> cameras;
    std::unique_ptr<capture::PassRecorder> recorder;
    int recordingPass = 0;
    std::optional<song::SampleCount> preRollPoint; // the running Pass's record point, when it used pre-roll
    video::VideoPlayback playback;
    std::optional<exporting::ExportRequest> exportWaiting; // until the audio thread takes the empty plan
    std::optional<exporting::MovieSource> movieWaiting;    // with exportWaiting, for an MP4
    std::unique_ptr<exporting::ExportJob> exportJob;
    std::optional<juce::File> lastExport;
};
} // namespace overdub::ui
