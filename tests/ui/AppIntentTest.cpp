#include "ui/Workspace.h"

#include "engine/DeviceSettings.h"
#include "engine/LatencyCalibration.h"
#include "song/FakeSeparationHelper.h"
#include "ui/CalibrationText.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using overdub::core::AbsolutePath;
using overdub::core::ScopedLogCapture;
using overdub::ui::AppIntent;
using overdub::ui::ClapMeasured;
using overdub::ui::LatencyMeasured;
using overdub::ui::LatencyMeasuring;
using overdub::ui::testing::click;
using overdub::ui::testing::contains;
using overdub::ui::testing::eventsNamed;
using overdub::ui::testing::isShown;
using overdub::ui::testing::messageCodes;
using overdub::ui::testing::WorkspaceFixture;
using namespace overdub::ui::intent;
using namespace std::chrono_literals;

// The App group of Intents (issue #170, ADR 0008), each run through MainComponent::run as the
// control socket runs it, on a window with its Workspace, the test LiveAudio and temp files.
namespace
{
AbsolutePath pathIn (const WorkspaceFixture& f, const std::string& name)
{
    return AbsolutePath { f.folder.getChildFile (name).getFullPathName().toStdString() };
}

juce::File fileOf (const AbsolutePath& path)
{
    return juce::File (juce::String::fromUTF8 (path.value.c_str()));
}

// The open Song's package, as the window shows it (null: the no-Song page).
juce::File shownPackage (WorkspaceFixture& f)
{
    if (f.content.currentSong() == nullptr)
        return {};
    CHECK_FALSE (isShown (f.content.noSongPage));
    return f.content.currentSong()->package().location();
}

void startSeparation (WorkspaceFixture& f)
{
    auto log = std::make_shared<overdub::song::testing::FakeHelperLog>();
    f.song().setSeparationTools (overdub::song::SeparationTools {
        .model = overdub::song::testing::writeStandInModel (f.folder),
        .startHelper = overdub::song::testing::fakeSeparationHelper ({}, log),
        .workFolder = f.folder.getChildFile ("work"),
    });
    REQUIRE (f.song().startSeparation (f.song().song().tracks.front().id).has_value());
}

// Polls as the window's timers would until the loopback run has ended and the Song is bound again.
void finishLatencyRun (WorkspaceFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (! f.workspace->isIdle() && std::chrono::steady_clock::now() < deadline)
    {
        f.audio().run (8);
        f.workspace->tick();
    }
    REQUIRE (f.workspace->isIdle());
}
} // namespace

//==============================================================================
// Songs

TEST_CASE ("new_song_at creates a package at the path, at the device's rate, and the window shows it", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    const auto path = pathIn (f, "First.overdub");
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { NewSongAt { path } });

    REQUIRE (result.has_value());
    CHECK (fileOf (path).getChildFile ("song.json").existsAsFile());
    CHECK (shownPackage (f) == fileOf (path));
    CHECK (f.song().song().sampleRate == f.audio().sampleRate());
    const auto ran = eventsNamed (capture, "ui.intent_run");
    REQUIRE (ran.size() == 1);
    CHECK (ran[0]["op"] == "new_song_at");
    CHECK (ran[0]["origin"] == "control");
}

TEST_CASE ("new_song_at where something exists presents song.package_exists and keeps the open Song", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    const auto path = pathIn (f, "First.overdub");
    REQUIRE (f.run (AppIntent { NewSongAt { path } }).has_value());
    REQUIRE (f.run (AppIntent { NewSongAt { pathIn (f, "Second.overdub") } }).has_value());
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { NewSongAt { path } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.package_exists");
    CHECK (contains (messageCodes (capture), "song.package_exists"));
    CHECK (shownPackage (f) == fileOf (pathIn (f, "Second.overdub")));
}

TEST_CASE ("open_song_at opens the package at the path and the window shows it; the open Song again changes nothing", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    const auto first = pathIn (f, "First.overdub");
    REQUIRE (f.run (AppIntent { NewSongAt { first } }).has_value());
    REQUIRE (f.run (AppIntent { NewSongAt { pathIn (f, "Second.overdub") } }).has_value());

    REQUIRE (f.run (AppIntent { OpenSongAt { first } }).has_value());
    CHECK (shownPackage (f) == fileOf (first));

    const auto* openSong = f.workspace->openSong();
    REQUIRE (f.run (AppIntent { OpenSongAt { first } }).has_value());
    CHECK (f.workspace->openSong() == openSong);
}

TEST_CASE ("open_song_at a path that is not a Song presents song.package_not_found and shows no Song", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { OpenSongAt { pathIn (f, "Missing.overdub") } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "song.package_not_found");
    CHECK (contains (messageCodes (capture), "song.package_not_found"));
    CHECK (f.content.currentSong() == nullptr);
    CHECK (isShown (f.content.noSongPage));
}

TEST_CASE ("duplicate_song_to clones the open Song to the path and opens the copy; the original stays", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    const auto original = pathIn (f, "First.overdub");
    const auto copy = pathIn (f, "First copy.overdub");
    REQUIRE (f.run (AppIntent { NewSongAt { original } }).has_value());

    REQUIRE (f.run (AppIntent { DuplicateSongTo { copy } }).has_value());

    CHECK (shownPackage (f) == fileOf (copy));
    CHECK (fileOf (copy).getChildFile ("song.json").existsAsFile());
    CHECK (fileOf (original).getChildFile ("song.json").existsAsFile());
}

TEST_CASE ("duplicate_song_to with no Song open answers ui.no_song_open, presenting nothing", "[ui][workspace][app_intent]")
{
    WorkspaceFixture f;
    f.launch();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { DuplicateSongTo { pathIn (f, "Copy.overdub") } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_song_open");
    CHECK (messageCodes (capture).empty());
    CHECK_FALSE (fileOf (pathIn (f, "Copy.overdub")).exists());
}

TEST_CASE ("opening, creating and duplicating Songs are refused while a Pass runs, saying so", "[ui][workspace][app_intent][record]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().record().has_value());
    ScopedLogCapture capture;

    for (const auto& songIntent : { AppIntent { NewSongAt { pathIn (f, "New.overdub") } },
                                    AppIntent { OpenSongAt { pathIn (f, "New.overdub") } },
                                    AppIntent { DuplicateSongTo { pathIn (f, "Copy.overdub") } } })
    {
        const auto result = f.run (songIntent);
        REQUIRE_FALSE (result.has_value());
        CHECK (result.error().code == "ui.stop_recording_first");
    }

    CHECK (f.song().isRecording());
    CHECK (std::ranges::count (messageCodes (capture), std::string ("ui.stop_recording_first")) == 3);
    CHECK_FALSE (fileOf (pathIn (f, "New.overdub")).exists());
    CHECK_FALSE (fileOf (pathIn (f, "Copy.overdub")).exists());
}

//==============================================================================
// convert_sample_rate

TEST_CASE ("convert_sample_rate from the mismatch Notice waits for a latency measurement, as from the menu", "[ui][workspace][app_intent][conversion]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong (44100); // the device runs at 48 kHz only
    REQUIRE (isShown (f.content.noticeLine.actionButton));
    REQUIRE (f.run (AppIntent { MeasureLatency {} }).has_value());
    ScopedLogCapture capture;

    click (f.content.noticeLine.actionButton);
    const auto fromMenu = f.content.run (AppIntent { ConvertSampleRate { 48000 } });

    CHECK_FALSE (f.song().isConverting());
    REQUIRE_FALSE (fromMenu.has_value());
    CHECK (fromMenu.error().code == "ui.calibration_running");
    CHECK (std::ranges::count (messageCodes (capture), std::string ("ui.calibration_running")) == 2);
    CHECK (f.song().song().sampleRate == 44100);
}

TEST_CASE ("convert_sample_rate with no Song open answers ui.no_song_open", "[ui][workspace][app_intent][conversion]")
{
    WorkspaceFixture f;
    f.launch();

    const auto result = f.run (AppIntent { ConvertSampleRate { 44100 } });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_song_open");
}

//==============================================================================
// Settings

TEST_CASE ("show_settings asks the window to show and hide the Settings window, which then shows the device and calibrations", "[ui][workspace][app_intent][settings]")
{
    WorkspaceFixture f;
    f.launch (false);

    REQUIRE (f.run (AppIntent { ShowSettings { true } }).has_value());
    CHECK (f.settingsShown == std::vector<bool> { true });
    CHECK (f.calibrationPanel.deviceLabel.getText() == "Scarlett 2i2 USB > Scarlett 2i2 USB, 48000 Hz");
    CHECK (f.devicePanel.inputPicker.getText() == "Scarlett 2i2 USB");

    REQUIRE (f.run (AppIntent { ShowSettings { false } }).has_value());
    CHECK (f.settingsShown == std::vector<bool> { true, false });
}

TEST_CASE ("show_settings is refused while a Pass runs; hiding the Settings window never is", "[ui][workspace][app_intent][settings][record]")
{
    WorkspaceFixture f;
    f.launch (false);
    f.openSong();
    REQUIRE (f.song().record().has_value());
    ScopedLogCapture capture;

    const auto shown = f.run (AppIntent { ShowSettings { true } });
    REQUIRE_FALSE (shown.has_value());
    CHECK (shown.error().code == "ui.stop_recording_first");
    CHECK (contains (messageCodes (capture), "ui.stop_recording_first"));
    CHECK (f.settingsShown.empty());

    CHECK (f.run (AppIntent { ShowSettings { false } }).has_value());
    CHECK (f.settingsShown == std::vector<bool> { false });
}

TEST_CASE ("apply_device_settings saves the choice and opens the audio again with it; the panel's Apply runs it", "[ui][workspace][app_intent][settings]")
{
    WorkspaceFixture f;
    f.machine.offeredRates = { 44100, 48000 };
    f.launch();
    f.openSong();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ApplyDeviceSettings { "Fake", "Scarlett 2i2 USB", "Scarlett 2i2 USB", 48000, 256 } });

    REQUIRE (result.has_value());
    REQUIRE (f.opened.size() == 2);
    CHECK (f.song().audio().engine == f.audio().engine());
    const auto saved = overdub::engine::readDeviceSettings (f.files.deviceSettings);
    REQUIRE (saved.has_value());
    CHECK (*saved == overdub::engine::DeviceSettings { "Fake", "Scarlett 2i2 USB", "Scarlett 2i2 USB", 48000, 256 });
    const auto changed = eventsNamed (capture, "ui.device_choice_changed");
    REQUIRE (changed.size() == 1);
    CHECK (changed[0]["sample_rate"] == 48000);
    CHECK (changed[0]["buffer_size"] == 256);

    click (f.devicePanel.applyButton);
    const auto ran = eventsNamed (capture, "ui.intent_run");
    REQUIRE (ran.size() == 2);
    CHECK (ran[1]["op"] == "apply_device_settings");
    CHECK (ran[1]["origin"] == "user");
    CHECK (f.opened.size() == 3);
}

//==============================================================================
// Calibration

TEST_CASE ("measure_latency works with the Settings window closed, and the panel shows the same result when opened", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.launch (false);
    f.openSong();
    f.runBlocks (1);

    REQUIRE (f.run (AppIntent { MeasureLatency {} }).has_value());
    CHECK (std::holds_alternative<LatencyMeasuring> (f.workspace->calibration().latency));
    CHECK_FALSE (f.workspace->isIdle());
    finishLatencyRun (f);

    const auto* measured = std::get_if<LatencyMeasured> (&f.workspace->calibration().latency);
    REQUIRE (measured != nullptr);
    REQUIRE (measured->outcome.has_value());
    REQUIRE (measured->toStore.has_value());
    CHECK (f.song().audio().engine == f.audio().engine());
    CHECK (f.settingsShown.empty());

    REQUIRE (f.run (AppIntent { ShowSettings { true } }).has_value());
    CHECK (f.calibrationPanel.latencyResultLabel.getText().toStdString() == overdub::ui::latencyResultText (*measured->outcome, f.audio().sampleRate()));
    REQUIRE (isShown (f.calibrationPanel.saveLatencyButton));

    const auto offered = measured->toStore->offsetSamples;
    click (f.calibrationPanel.saveLatencyButton);
    CHECK (f.audio().storedCalibrationOffset() == offered);
}

TEST_CASE ("measure_latency without an audio device says so and runs nothing", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.machine.deviceAvailable = false;
    f.launch();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { MeasureLatency {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_audio_device");
    CHECK (contains (messageCodes (capture), "ui.no_audio_device"));
    CHECK (std::holds_alternative<std::monostate> (f.workspace->calibration().latency));
    CHECK (f.workspace->isIdle());
}

TEST_CASE ("save_latency_calibration stores the offset, and the panel shows the device calibrated", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.launch();

    REQUIRE (f.run (AppIntent { SaveLatencyCalibration { -821 } }).has_value());

    const auto stored = overdub::engine::findCalibrationOffset (f.files.latencyCalibration, f.audio().calibrationKey());
    REQUIRE (stored.has_value());
    CHECK (*stored == std::optional<overdub::song::SampleCount> (-821));
    CHECK (f.workspace->calibration().status.latencyOffset == -821);
    CHECK (f.calibrationPanel.latencyStatusLabel.getText() == "Calibrated: -821 samples (-17.1 ms).");
}

TEST_CASE ("measure_clap_pass measures the open Song's newest Video Take into the calibration state", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.launch (false);
    f.openSong(); // no Pass has a Video Take
    ScopedLogCapture capture;

    REQUIRE (f.run (AppIntent { MeasureClapPass {} }).has_value());

    const auto* measured = std::get_if<ClapMeasured> (&f.workspace->calibration().clap);
    REQUIRE (measured != nullptr);
    REQUIRE_FALSE (measured->outcome.has_value());
    CHECK (measured->outcome.error().code == "capture.clap_no_video_take");
    CHECK_FALSE (measured->toStore.has_value());
    CHECK (eventsNamed (capture, "capture.clap_no_video_take").size() == 1);

    REQUIRE (f.run (AppIntent { ShowSettings { true } }).has_value());
    CHECK (f.calibrationPanel.clapResultLabel.getText() == "No Pass has a Video Take yet. Arm the camera and record a Pass clapping in frame.");
}

TEST_CASE ("measure_clap_pass with no Song open answers ui.no_song_open", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.launch();

    const auto result = f.run (AppIntent { MeasureClapPass {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_song_open");
}

TEST_CASE ("save_camera_calibration stores it for the camera and output device, and the state says what was saved", "[ui][workspace][app_intent][calibration]")
{
    WorkspaceFixture f;
    f.launch (false);

    REQUIRE (f.run (AppIntent { SaveCameraCalibration { "camera-facetime", "Scarlett 2i2 USB", 35.0 } }).has_value());

    const auto stored = overdub::capture::findCameraCalibration (f.files.cameraCalibration, { "camera-facetime", "Scarlett 2i2 USB" });
    REQUIRE (stored.has_value());
    CHECK (*stored == std::optional<double> (35.0));
    const auto* saved = std::get_if<overdub::ui::CameraSaved> (&f.workspace->calibration().clap);
    REQUIRE (saved != nullptr);
    CHECK (saved->stored == SaveCameraCalibration { "camera-facetime", "Scarlett 2i2 USB", 35.0 });
}

//==============================================================================
// quit

TEST_CASE ("quit asks the window to quit; a running Pass does not stop it", "[ui][workspace][app_intent][record]")
{
    WorkspaceFixture f;
    f.launch();
    REQUIRE (f.run (AppIntent { Quit {} }).has_value());
    CHECK (f.quits == 1);

    f.openSong();
    REQUIRE (f.song().record().has_value());
    REQUIRE (f.run (AppIntent { Quit {} }).has_value());
    CHECK (f.quits == 2);
}

TEST_CASE ("the Settings window's Apply and Measure, outside the run dialog, present the Separation running Notice", "[ui][workspace][app_intent][separation]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.run (AppIntent { ShowSettings { true } }).has_value());
    startSeparation (f);
    f.content.tick();
    REQUIRE (isShown (f.content.runDialog));
    ScopedLogCapture capture;

    click (f.devicePanel.applyButton);
    click (f.calibrationPanel.measureLatencyButton);

    CHECK (std::ranges::count (messageCodes (capture), std::string ("ui.separation_running")) == 2);
    CHECK (eventsNamed (capture, "ui.device_choice_changed").empty());
    CHECK (f.song().isSeparating());
}

TEST_CASE ("quit is refused while a Separation runs, as command-Q is, and allowed once it is cancelled", "[ui][workspace][app_intent][separation]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    startSeparation (f);
    f.content.tick();
    REQUIRE (isShown (f.content.runDialog));
    ScopedLogCapture capture;

    const auto refused = f.run (AppIntent { Quit {} });

    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "ui.separation_running");
    CHECK (contains (messageCodes (capture), "ui.separation_running"));
    CHECK (f.quits == 0);
    CHECK (f.song().isSeparating());
    CHECK (f.run (AppIntent { ShowSettings { false } }).has_value());
    CHECK (f.settingsShown == std::vector<bool> { false });

    click (f.content.runDialog.cancelButton);
    REQUIRE (f.run (AppIntent { Quit {} }).has_value());
    CHECK (f.quits == 1);
}
