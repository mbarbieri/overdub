#pragma once

#include "capture/Camera.h"
#include "capture/CameraCalibration.h"
#include "capture/LoopbackRun.h"
#include "plugin/AudioUnitLoader.h"
#include "song/SongPackage.h"
#include "ui/CalibrationPanel.h"
#include "ui/DeviceSettingsPanel.h"
#include "ui/LiveAudio.h"
#include "ui/MainComponent.h"
#include "ui/OpenSong.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

// What the main window does with the live audio and the open Song (issue #105), apart from its
// menus, dialogs and settings window: it owns the LiveAudio, the open Song (a ui::OpenSong), a
// running loopback calibration and a device change not answered yet, and presents what the user
// needs to know through the MainComponent and the settings panels.
//
// Opening another Song, changing the device and closing each finish a running Pass first
// (ui::OpenSong's invariant). While a Pass or a loopback run runs, they are refused with
// ui.stop_recording_first or ui.calibration_running (idleOrNotice).
//
// Device changes (issue #41, ADR 0004 amendment): opening a Song switches the device to the Song's
// rate when the device offers it (a new LiveAudio); otherwise the Song opens without audio and
// shows ui.sample_rate_mismatch. A device change the LiveAudio reports is answered by tick() with a
// new LiveAudio under the same rule: at once when the device was lost or reconfigured (a Pass it cut
// short is finished first by the window's poll, its Alert shown; an export is stopped), and once no
// Pass, export or loopback run is running when the saved choice came back or a device appeared.
// A lost device that falls back to another shows ui.audio_device_lost. Logs ui.audio_reopened
// (info: change).
//
// Calibration (issue #39): a device opened without a stored latency calibration shows the
// ui.latency_not_calibrated Notice. measureLatency() detaches the open Song from the engine and runs
// a capture::LoopbackRun on the live device (polled by tick()); the Song is bound again only once
// the audio thread has taken the newest plan, so no plan renders the Song's plugins while they are
// prepared again (OpenSong::attachAudio's invariant). Saving a latency result writes it to the
// latency calibration file and to the LiveAudio, the offset's one owner, from which the open Song
// takes it for its next Pass. measureClapPass() measures the open Song's newest Video Take; saving
// it writes the camera calibration for that camera and the output device its Pass played through
// (issue #68), then the panel is refreshed from the files and confirms the save (issue #88).
//
// Message thread only. The MainComponent, the camera and whatever the Instantiator uses must
// outlive the Workspace; the settings panels must outlive their attachment.
namespace overdub::ui
{
// The per-machine files the Workspace reads and writes (the app's defaults, a temp folder in tests).
struct WorkspaceFiles
{
    juce::File deviceSettings;
    juce::File latencyCalibration;
    juce::File cameraCalibration;

    static WorkspaceFiles defaults();
};

class Workspace
{
public:
    // Takes the audio opened at launch (no Song is open yet) and shows ui.no_audio_device or
    // ui.latency_not_calibrated when they apply. `openAudio` opens it again on every device change.
    Workspace (MainComponent& content,
               std::unique_ptr<LiveAudio> audio,
               OpenLiveAudio openAudio,
               capture::Camera* camera,
               plugin::Instantiator instantiate,
               WorkspaceFiles files);
    // Stops a loopback run and closes the Song (finishing a running Pass while the device runs),
    // without calling onOpenSongChanged.
    ~Workspace();

    Workspace (const Workspace&) = delete;
    Workspace& operator= (const Workspace&) = delete;

    // Null while no Song is open.
    OpenSong* openSong() const { return song.get(); }
    LiveAudio& liveAudio() const { return *live; }
    // True when neither a Pass nor a loopback run is running.
    bool isIdle() const;
    // isIdle(), or false after showing the Notice that says what runs.
    bool idleOrNotice();

    // Closes the open Song (finishing a running Pass) and shows this one (see the header comment
    // for its rate). The package's open Notices are shown.
    void showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices);
    // Opens the package at `chosen` (ui/SongFiles.h) and shows it; the open Song again changes
    // nothing. An Error is presented.
    void openSongFrom (const juce::File& chosen);
    void closeSong();

    // The settings window's panels (null to detach): their gestures act here, and they show the
    // device choices and calibrations in use.
    void attachSettingsPanels (DeviceSettingsPanel* devices, CalibrationPanel* calibration);
    // Shows the current device choices and calibrations in the attached panels.
    void refreshSettingsPanels();

    // Saves this device choice and opens the audio again with it.
    void applyDeviceSettings (const engine::DeviceSettings& settings);
    void measureLatency();
    void saveLatencyCalibration (song::SampleCount offsetSamples);
    void measureClapPass();
    void saveCameraCalibration (const capture::CameraCalibrationKey& key, double offsetMs);

    // Call regularly (window timer): answers a device change the LiveAudio reported once it may
    // (see the header comment), and advances a loopback run (`now` is its clock).
    void tick (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

    // Called after the open Song changed (opened, closed): the window's title follows it.
    std::function<void()> onOpenSongChanged;

private:
    // Closes the live audio and opens it again (at the open Song's rate when offered), binds the
    // open Song and shows what the user needs to know.
    void reopenAudio();
    void watchDevices();
    void answerDeviceChange();
    void pollLatencyRun (song::SongPackage::Clock::time_point now);
    bool latencyMeasuring() const;
    // Finishes a running Pass and unbinds the engine; errors go to the content.
    void detachAudio();
    void showRateMismatch();
    void showLatencyNoticeIfUncalibrated();
    CalibrationStatus calibrationStatus() const;
    void refreshCalibrationPanel();
    std::optional<int> openSongRate() const;

    MainComponent& content;
    OpenLiveAudio openAudio;
    capture::Camera* camera;
    plugin::Instantiator instantiate;
    WorkspaceFiles files;
    DeviceSettingsPanel* devicePanel = nullptr;
    CalibrationPanel* calibrationPanel = nullptr;
    // The device change reported and not answered yet (the newest wins).
    std::optional<engine::DeviceChange> pendingDeviceChange;
    // Invariant: declared before `song`, so the device keeps running while a Pass finishes.
    std::unique_ptr<LiveAudio> live;
    std::unique_ptr<OpenSong> song;
    // After `live` and `song`: destroyed first, while the device still runs to stop its Pass.
    std::unique_ptr<capture::LoopbackRun> latencyRun;
    // The run's outcome, held until the audio thread lets go of the plans and the Song is bound again.
    std::optional<core::Expected<capture::LoopbackMeasurement>> latencyOutcome;
};
} // namespace overdub::ui
