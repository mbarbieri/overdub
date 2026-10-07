#pragma once

#include "capture/PassRecorder.h"
#include "core/Error.h"
#include "engine/Engine.h"
#include "song/SongPackage.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

// One open Song bound to the live engine (issue #27): what the main window's gestures do. It owns
// the package, keeps the engine playing the current Song (reloaded after every change), and runs
// at most one Pass (capture/PassRecorder.h).
//
// Invariant: every Pass this session starts is finished by it. stop() asks the audio thread to
// end the Pass; poll() (from the window's timer) finishes it once the audio thread has; detaching
// the audio or destroying the session waits for the audio thread and finishes it there.
//
// While a Pass runs, only stop, play/stop and rewind act: undo, redo, import, arm, input and
// Monitoring are ignored, so the Song and the engine's plan stay as the Pass started.
//
// Message thread only.
namespace overdub::ui
{
// The audio a session plays and records through. A null engine means no device is open.
struct AudioBinding
{
    engine::Engine* engine = nullptr;
    capture::RecordLatency latency;
    // The device's active input channel names, in the order process() receives them.
    std::vector<std::string> inputChannelNames;
};

class SongSession
{
public:
    // Loads the Song into the engine, with Monitoring on for no Track, at the Song's playhead.
    SongSession (std::unique_ptr<song::SongPackage> package, AudioBinding audio);
    // detachAudio() (errors logged), then the package writes what is pending as it closes.
    ~SongSession();

    SongSession (const SongSession&) = delete;
    SongSession& operator= (const SongSession&) = delete;

    const song::Song& song() const { return package_->song(); }
    song::SongPackage& package() { return *package_; }
    const song::SongPackage& package() const { return *package_; }
    const AudioBinding& audio() const { return binding; }

    // Binds a (new) engine: loads the Song and seeks to its playhead. Call detachAudio() first.
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
    // runs. Errors: ui.no_audio_device, or PassRecorder::start's (capture.nothing_armed, ...).
    core::Expected<void> record();
    // Stops the transport and asks the audio thread to end a running Pass.
    void stop();
    // Plays, or stops (as stop()) when playing or recording.
    void playOrStop();
    // To the start of the Song; ends a running Pass (as stop()).
    void rewind();

    // Errors: SongPackage::undo / redo's (song.history_empty when there is no step).
    core::Expected<void> undo();
    core::Expected<void> redo();

    void setArmed (const song::Id& trackId, bool armed);
    void setInputChannels (const song::Id& trackId, std::vector<int> inputChannels);
    // Monitoring is transient (not in the Song): hearing an Armed Track's live input.
    void setMonitoring (const song::Id& trackId, bool on);
    bool isMonitoring (const song::Id& trackId) const { return monitored.contains (trackId); }

    // One Backing Track import per file (SongPackage::importBackingTracks); empty during a Pass.
    std::vector<core::Expected<song::Id>> importFiles (const std::vector<juce::File>& files);

    // Call regularly (window timer): finishes a Pass the audio thread has ended, saves the
    // playhead once the transport is stopped, and writes a due song.json. Returns the errors,
    // not logged.
    std::vector<core::Error> poll (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

private:
    void reloadEngine();
    std::vector<core::Error> finishPass (core::Expected<capture::RecordedPass> finished);

    std::unique_ptr<song::SongPackage> package_;
    AudioBinding binding;
    std::set<song::Id> monitored;
    std::unique_ptr<capture::PassRecorder> recorder;
    int recordingPass = 0;
};
} // namespace overdub::ui
