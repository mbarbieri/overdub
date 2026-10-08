#pragma once

#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "plugin/AudioUnitLoader.h"
#include "plugin/InsertHost.h"
#include "song/SongPackage.h"

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
// Insert Chains (issue #33): it hosts the Song's plugins in a plugin::InsertHost, created through
// the Instantiator it is given (out-of-process AUs in the app). The engine plays each plugin once
// it has loaded; a plugin that loads or changes its latency rebuilds the plan at the next poll()
// (after the Pass, while one runs). The package captures every plugin's state into its slot on
// each history step and debounced write; a parameter change schedules that write. Undo keeps a
// surviving slot's plugin as it is and loads a slot it brings back with the snapshot's state.
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

    // True from record() until poll() or detachAudio() has finished the Pass.
    bool isRecording() const { return recorder != nullptr; }
    // The number of the Pass being recorded (from 1); 0 when not recording.
    int recordingPassNumber() const { return recordingPass; }
    bool isPlaying() const;
    song::SampleCount position() const;

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

    // Pre-roll is a Song setting outside history: written to song.json, never an undo step.
    // Ignored while a Pass runs.
    void setPreRoll (song::PreRoll preRoll);

    void setArmed (const song::Id& trackId, bool armed);
    void setInputChannels (const song::Id& trackId, std::vector<int> inputChannels);
    // Monitoring is transient (not in the Song): hearing an Armed Track's live input.
    void setMonitoring (const song::Id& trackId, bool on);
    bool isMonitoring (const song::Id& trackId) const { return monitored.contains (trackId); }

    // One Backing Track import per file (SongPackage::importBackingTracks); empty during a Pass.
    std::vector<core::Expected<song::Id>> importFiles (const std::vector<juce::File>& files);

    // Call regularly (window timer): finishes a Pass the audio thread has ended, rebuilds the plan
    // when a plugin loaded or changed its latency (not during a Pass), schedules a write when a
    // plugin parameter changed, saves the playhead once the transport is stopped, and writes a due
    // song.json. Returns the errors, not logged.
    std::vector<core::Error> poll (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

private:
    void reloadEngine();
    // After a history change: setMix when only the mix changed since `before`, else reloadEngine.
    void updateEngine (const song::Song& before);
    std::vector<core::Error> finishPass (core::Expected<capture::RecordedPass> finished);
    void capturePluginState (song::Song& song);
    static int blockSizeOf (const AudioBinding& audio);

    std::unique_ptr<song::SongPackage> package_;
    // After package_: destroyed first, once the destructor has captured its state and unhooked it.
    plugin::InsertHost inserts;
    AudioBinding binding;
    std::set<song::Id> monitored;
    std::map<song::Id, song::Peaks> peaksByMediaId; // media is immutable, so entries never go stale
    std::unique_ptr<capture::PassRecorder> recorder;
    int recordingPass = 0;
    std::optional<song::SampleCount> preRollPoint; // the running Pass's record point, when it used pre-roll
};
} // namespace overdub::ui
