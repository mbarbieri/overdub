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
#include <string>
#include <string_view>
#include <vector>

// What the main window does with the live audio and the open Song (issue #105), apart from its
// menus, dialogs and settings window: it owns the LiveAudio, the open Song (a ui::OpenSong), a
// running loopback calibration, the calibration results and a device change not answered yet, and
// presents what the user needs to know through the MainComponent and the settings panels.
//
// It owns the App group of Intents (issue #170, ADR 0008): run (AppIntent), which the
// MainComponent's run hands every one of them, from a menu, a dialog's answer, a settings panel or
// the control socket. The window keeps only what needs a native dialog or window, and the two
// things that cannot live in src/ui: showing the Settings window (onShowSettings) and quitting the
// app (onQuit).
//
// Opening another Song, changing the device and closing each finish a running Pass first
// (ui::OpenSong's invariant). While a Pass, a Separation, a conversion or a loopback run runs,
// new_song_at, open_song_at, duplicate_song_to, convert_sample_rate, show_settings (shown),
// apply_device_settings, measure_latency, save_latency_calibration, measure_clap_pass and
// save_camera_calibration are refused with the Notice that says what runs
// (ui.stop_recording_first, ui.separation_running, ui.conversion_running, ui.calibration_running).
// quit is refused only while a Separation runs (ui.separation_running: its dialog is modal and
// closes only on success, Cancel or failure); a Pass is finished on quit as on close.
//
// Device changes (issue #41, ADR 0004 amendment): opening a Song switches the device to the Song's
// rate when the device offers it (a new LiveAudio); otherwise the Song opens without audio and
// shows ui.sample_rate_mismatch. A device change the LiveAudio reports is answered by tick() with a
// new LiveAudio under the same rule: at once when the device was lost or reconfigured (a Pass it cut
// short is finished first by the window's poll, its Alert shown; an export is stopped), and once no
// Pass, export or loopback run is running when the saved choice came back or a device appeared.
// A lost device that falls back to another shows ui.audio_device_lost. Logs ui.audio_reopened
// (info: change). When the open Song's rate changes (a sample rate conversion, or an undo or redo
// across one, issue #160), tick() opens the audio again under the same rule (change
// song_rate_changed).
//
// Calibration (issues #39, #170): a device opened without a stored latency calibration shows the
// ui.latency_not_calibrated Notice. measure_latency detaches the open Song from the engine and runs
// a capture::LoopbackRun on the live device (polled by tick()); the Song is bound again only once
// the audio thread has taken the newest plan, so no plan renders the Song's plugins while they are
// prepared again (OpenSong::attachAudio's invariant). Saving a latency result writes it to the
// latency calibration file and to the LiveAudio, the offset's one owner, from which the open Song
// takes it for its next Pass. measure_clap_pass measures the open Song's newest Video Take; saving
// it writes the camera calibration for that camera and the output device its Pass played through
// (issue #68). The results are the Workspace's CalibrationState, whether or not the Settings window
// is open: the calibration panel shows it when attached. A latency result is dropped when another
// device opens, a clap result when the Song closes.
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
    // Separation (issue #114): the installed model and the overdub-separate helper. Empty: a
    // Separation is unavailable (song.separation_unavailable or song.separation_model_missing).
    juce::File separationModel = {};
    juce::File separationHelper = {};

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
    // True when no Pass, Separation, conversion or loopback run is running; an export may be.
    bool nothingRunsButAnExport() const;
    // nothingRunsButAnExport(), or false after showing the Notice that says what runs (before
    // opening a dialog).
    bool nothingRunsButAnExportOrNotice();

    // Runs one App Intent (see the header comment). An Error is presented by its tier, except
    // ui.no_song_open, which is logged at debug.
    IntentResult run (const AppIntent& intent);

    // Closes the open Song (finishing a running Pass) and shows this one (see the header comment
    // for its rate). The package's open Notices are shown.
    void showSong (std::unique_ptr<song::SongPackage> package, const std::vector<song::Notice>& notices);
    void closeSong();

    // The settings window's panels (null to detach): their gestures run App Intents, and they show
    // the device choices and the CalibrationState at once and on every change.
    void attachSettingsPanels (DeviceSettingsPanel* devices, CalibrationPanel* calibration);
    const CalibrationState& calibration() const { return calibrationState; }

    // What runs now: the open Song's activity, else a loopback run (calibration_latency), else idle.
    ActivityKind activityKind() const;
    // Every activity started and finished in this Workspace, the open Songs' included.
    const ActivityLog& activities() const { return activityLog; }
    // The code run would refuse this App Intent with now because something runs; none when it
    // would run.
    std::optional<std::string> refusalCodeOf (const AppIntent& intent) const;

    // Call regularly (window timer): answers a device change the LiveAudio reported once it may
    // (see the header comment), and advances a loopback run (`now` is its clock).
    void tick (song::SongPackage::Clock::time_point now = song::SongPackage::Clock::now());

    // Called after the open Song changed (opened, closed): the window's title follows it.
    std::function<void()> onOpenSongChanged;
    // show_settings: shows (true) or hides the Settings window, which attaches its panels.
    std::function<void (bool shown)> onShowSettings;
    // quit, once allowed: quits the app.
    std::function<void()> onQuit;

private:
    // The Notice code of what runs, none when nothing runs but an export.
    std::optional<std::string> runningCode() const;
    // `op` refused because something runs: the Error, presented as its Notice.
    IntentResult refuseWhileRunning (const std::string& code, std::string_view op);
    IntentResult newSongAt (const juce::File& chosen);
    // The open Song again changes nothing.
    IntentResult openSongFrom (const juce::File& chosen);
    IntentResult duplicateSongTo (const juce::File& chosen);
    IntentResult convertSampleRate (int toRate);
    IntentResult showSettings (bool shown);
    // Saves this device choice and opens the audio again with it.
    IntentResult applyDeviceSettings (const engine::DeviceSettings& settings);
    IntentResult measureLatency();
    IntentResult saveLatencyCalibration (song::SampleCount offsetSamples);
    IntentResult measureClapPass();
    // key.outputDevice is the clap Pass's (its Video Take's), not necessarily the one open now.
    IntentResult saveCameraCalibration (const capture::CameraCalibrationKey& key, double offsetMs);
    IntentResult quit();
    // A result handled here: presented by its tier, then returned.
    IntentResult presented (core::Error error);

    // Closes the live audio and opens it again (at the open Song's rate when offered), binds the
    // open Song and shows what the user needs to know.
    void reopenAudio();
    // Replaces the live audio with one opened for `songRate`; the latency result goes with the old one.
    void swapAudio (std::optional<int> songRate);
    void watchDevices();
    void answerSongRateChange();
    // ui.device_rate_substituted (a Notice) when a picked rate opened at another one (issue #159).
    void showRateSubstitution (int requestedRate);
    void answerDeviceChange();
    void pollLatencyRun (song::SongPackage::Clock::time_point now);
    bool latencyMeasuring() const;
    // Finishes a running Pass and unbinds the engine; errors go to the content.
    void detachAudio();
    void showRateMismatch();
    void showLatencyNoticeIfUncalibrated();
    CalibrationStatus calibrationStatus() const;
    // Shows the current device choices and calibrations in the attached panels.
    void refreshSettingsPanels();
    // Reads the stored calibrations again and shows the CalibrationState in the attached panel.
    void refreshCalibration();
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
    // Before `song`, which counts its activities here until it is destroyed.
    ActivityLog activityLog;
    std::uint64_t latencyRunCounter = 0;
    // Invariant: declared before `song`, so the device keeps running while a Pass finishes.
    std::unique_ptr<LiveAudio> live;
    std::unique_ptr<OpenSong> song;
    // After `live` and `song`: destroyed first, while the device still runs to stop its Pass.
    std::unique_ptr<capture::LoopbackRun> latencyRun;
    // The run's outcome, held until the audio thread lets go of the plans and the Song is bound again.
    std::optional<core::Expected<capture::LoopbackMeasurement>> latencyOutcome;
    CalibrationState calibrationState;
};
} // namespace overdub::ui
