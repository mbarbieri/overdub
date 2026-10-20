#include "ui/Workspace.h"

#include "song/FakeSeparationHelper.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using overdub::engine::DeviceChange;
using overdub::exporting::ExportFormat;
using overdub::ui::AppIntent;
using overdub::ui::testing::choose;
using overdub::ui::testing::click;
using overdub::ui::testing::contains;
using overdub::ui::testing::eventsNamed;
using overdub::ui::testing::isShown;
using overdub::ui::testing::messageCodes;
using overdub::ui::testing::WorkspaceFixture;
using namespace overdub::ui::intent;
using namespace std::chrono_literals;

// The main window's device and calibration policy (issue #105), driven through the Workspace with
// the test LiveAudio (ui/WorkspaceFixture.h).
namespace
{
// apply_device_settings with the device choice in use.
ApplyDeviceSettings applyCurrent (WorkspaceFixture& f)
{
    const auto current = f.audio().deviceChoices().current;
    return ApplyDeviceSettings { current.deviceType, current.inputDevice, current.outputDevice, current.sampleRate, current.bufferSize };
}
} // namespace

TEST_CASE ("a device lost during an export is answered at once: the export stops and the Song plays on the reopened device", "[ui][workspace][device]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().startExport ({ ExportFormat::wav, f.folder.getChildFile ("Song.wav") }).has_value());
    REQUIRE (f.song().isExporting());
    overdub::core::ScopedLogCapture capture;

    f.audio().reportChange (DeviceChange::lost);
    f.workspace->tick();

    REQUIRE (f.opened.size() == 2);
    CHECK_FALSE (f.song().isExporting());
    const auto stopped = eventsNamed (capture, "ui.export_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["reason"] == "audio_detached");
    CHECK (f.song().audio().engine == f.audio().engine());
    const auto reopened = eventsNamed (capture, "ui.audio_reopened");
    REQUIRE (reopened.size() == 1);
    CHECK (reopened[0]["change"] == "lost");
    CHECK (contains (messageCodes (capture), "ui.audio_device_lost"));
}

TEST_CASE ("a device lost during a Pass reopens the audio only once the Pass is finished, after its Alert", "[ui][workspace][device][record]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().record().has_value());
    f.audio().run (10);
    overdub::core::ScopedLogCapture capture;

    f.audio().reportChange (DeviceChange::lost);
    f.workspace->tick();
    CHECK (f.opened.size() == 1); // the Pass the device cut short is still being finished

    f.content.tick(); // the window's poll finishes it
    CHECK_FALSE (f.song().isRecording());
    CHECK (contains (messageCodes (capture), "ui.audio_device_lost_during_pass"));
    CHECK (f.song().song().passes.size() == 1); // kept
    f.workspace->tick();

    REQUIRE (f.opened.size() == 2);
    CHECK (f.song().audio().engine == f.audio().engine());
}

TEST_CASE ("the saved device coming back during a Pass is answered once the Pass ends", "[ui][workspace][device][record]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().record().has_value());
    f.runBlocks (10);

    f.audio().reportChange (DeviceChange::savedChoiceAvailable);
    f.tick();
    f.runBlocks (5);
    CHECK (f.opened.size() == 1); // recording goes on, on the device it started on
    CHECK (f.song().isRecording());

    f.song().stop();
    f.audio().run (1);
    f.tick();

    CHECK_FALSE (f.song().isRecording());
    REQUIRE (f.opened.size() == 2);
    CHECK (f.song().audio().engine == f.audio().engine());
}

TEST_CASE ("a lost device falls back to another, and the saved device coming back is answered with a new LiveAudio", "[ui][workspace][device]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    overdub::core::ScopedLogCapture capture;

    f.machine.outputDevice = "MacBook Pro Speakers"; // the fallback the device manager opens
    f.audio().reportChange (DeviceChange::lost);
    f.workspace->tick();
    REQUIRE (f.opened.size() == 2);
    CHECK (f.audio().calibrationKey().outputDevice == "MacBook Pro Speakers");
    CHECK (contains (messageCodes (capture), "ui.audio_device_lost"));

    f.machine.outputDevice = "Scarlett 2i2 USB"; // plugged in again
    f.audio().reportChange (DeviceChange::savedChoiceAvailable);
    f.workspace->tick();

    REQUIRE (f.opened.size() == 3);
    CHECK (f.audio().calibrationKey().outputDevice == "Scarlett 2i2 USB");
    CHECK (f.song().audio().engine == f.audio().engine());
    const auto reopened = eventsNamed (capture, "ui.audio_reopened");
    REQUIRE (reopened.size() == 2);
    CHECK (reopened[1]["change"] == "saved_choice_available");
}

TEST_CASE ("a device that appears during an export is answered once the export ends", "[ui][workspace][device]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().startExport ({ ExportFormat::wav, f.folder.getChildFile ("Song.wav") }).has_value());

    f.audio().reportChange (DeviceChange::appeared);
    f.workspace->tick();
    CHECK (f.opened.size() == 1);

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (f.song().isExporting() && std::chrono::steady_clock::now() < deadline)
        f.runBlocks (1);
    REQUIRE_FALSE (f.song().isExporting());
    f.workspace->tick();

    CHECK (f.opened.size() == 2);
}

TEST_CASE ("a reconfigured device while idle is answered at once, and the Song plays on the reopened device", "[ui][workspace][device]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    overdub::core::ScopedLogCapture capture;

    f.audio().reportChange (DeviceChange::reconfigured);
    f.workspace->tick();

    REQUIRE (f.opened.size() == 2);
    CHECK (f.song().audio().engine == f.audio().engine());
    CHECK_FALSE (contains (messageCodes (capture), "ui.audio_device_lost"));
    f.runBlocks (1);
    f.song().playOrStop();
    f.runBlocks (2);
    CHECK (f.song().isPlaying());
}

TEST_CASE ("a Song at another rate opens on the device switched to that rate when the device offers it", "[ui][workspace][rate]")
{
    WorkspaceFixture f;
    f.machine.offeredRates = { 44100, 48000 };
    f.launch();

    f.openSong (44100);

    REQUIRE (f.opened.size() == 2);
    CHECK (f.openedForRates.back() == 44100);
    CHECK (f.audio().sampleRate() == 44100);
    CHECK_FALSE (f.song().audioProblem().has_value());
}

TEST_CASE ("a Song at a rate the device does not offer opens without audio and says so", "[ui][workspace][rate]")
{
    WorkspaceFixture f;
    f.launch();
    overdub::core::ScopedLogCapture capture;

    f.openSong (44100);

    CHECK (f.opened.size() == 1);
    REQUIRE (f.song().audioProblem().has_value());
    CHECK (f.song().audioProblem()->code == "ui.sample_rate_mismatch");
    CHECK (contains (messageCodes (capture), "ui.sample_rate_mismatch"));
}

TEST_CASE ("on a device that doesn't offer the Song's rate, opening and Record both say so and name both rates; no Pass starts", "[ui][workspace][rate]")
{
    WorkspaceFixture f;
    f.launch();
    overdub::core::ScopedLogCapture capture;
    const auto expected = juce::String ("This Song is at 44100 Hz, which the audio device (at 48000 Hz) doesn't offer, so it plays and records nothing. Use a device that offers 44100 Hz, or record in a new Song.");

    f.openSong (44100);
    CHECK (f.content.noticeLine.text() == expected);

    f.content.noticeLine.dismiss();
    REQUIRE (f.content.noticeLine.text().isEmpty());
    f.content.run (overdub::ui::SongIntent { Record {} });
    CHECK (f.content.noticeLine.text() == expected);
    CHECK_FALSE (f.song().isRecording());
    CHECK (f.song().song().passes.empty());

    std::vector<overdub::core::Fields> mismatches;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.sample_rate_mismatch")
            mismatches.push_back (event);
    REQUIRE (mismatches.size() == 2);
    for (const auto& event : mismatches)
    {
        CHECK (event.at ("song_rate") == 44100);
        CHECK (event.at ("device_rate") == overdub::engine::testing::testSampleRate);
        CHECK (event.at ("rate_offered") == false);
    }
}

TEST_CASE ("a latency calibration binds the Song again only once the audio thread has taken the newest plan", "[ui][workspace][calibration]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    f.runBlocks (1);

    f.run (AppIntent { MeasureLatency {} });
    CHECK (f.song().audio().engine == nullptr); // the run plays its own Song
    CHECK_FALSE (f.workspace->isIdle());

    // The device never called back since: the run gives up, but its plan is still pending.
    f.workspace->tick (overdub::song::SongPackage::Clock::now() + 1h);
    CHECK (f.song().audio().engine == nullptr);
    REQUIRE (f.audio().engine()->hasPendingPlan());

    f.audio().run (1); // the audio thread takes it
    f.workspace->tick();

    CHECK (f.song().audio().engine == f.audio().engine());
    CHECK (f.workspace->isIdle());
}

TEST_CASE ("a latency calibration measured on the loopback device is offered in the panel once the Song is bound again", "[ui][workspace][calibration]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    f.runBlocks (1);

    f.run (AppIntent { MeasureLatency {} });
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (! f.workspace->isIdle() && std::chrono::steady_clock::now() < deadline)
    {
        f.audio().run (8);
        f.workspace->tick();
    }

    REQUIRE (f.workspace->isIdle());
    CHECK (f.song().audio().engine == f.audio().engine());
    CHECK (overdub::ui::testing::isShown (f.calibrationPanel.saveLatencyButton));
}

TEST_CASE ("saving a latency calibration stores it for the device, and the next Pass and the next device opened use it", "[ui][workspace][calibration]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();

    f.run (AppIntent { SaveLatencyCalibration { -300 } });

    const auto stored = overdub::engine::findCalibrationOffset (f.files.latencyCalibration, f.audio().calibrationKey());
    REQUIRE (stored.has_value());
    CHECK (*stored == -300);
    CHECK (f.audio().storedCalibrationOffset() == -300);
    CHECK (f.song().audio().latency.calibrationOffsetSamples == -300);

    f.audio().reportChange (DeviceChange::reconfigured);
    f.workspace->tick();
    REQUIRE (f.opened.size() == 2);
    CHECK (f.audio().storedCalibrationOffset() == -300);
    CHECK (f.song().audio().latency.calibrationOffsetSamples == -300);
}

TEST_CASE ("an uncalibrated device shows ui.latency_not_calibrated at launch, a calibrated one does not", "[ui][workspace][calibration]")
{
    {
        overdub::core::ScopedLogCapture capture;
        WorkspaceFixture f;
        f.launch();
        CHECK (contains (messageCodes (capture), "ui.latency_not_calibrated"));
    }
    {
        WorkspaceFixture f;
        REQUIRE (overdub::engine::writeCalibrationOffset (f.files.latencyCalibration, { f.machine.inputDevice, f.machine.outputDevice, f.machine.deviceRate }, -250).has_value());
        overdub::core::ScopedLogCapture capture;
        f.launch();
        CHECK_FALSE (contains (messageCodes (capture), "ui.latency_not_calibrated"));
    }
}

TEST_CASE ("saving a camera calibration stores it under the clap Pass's output device, and the panel confirms it", "[ui][workspace][calibration]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    const overdub::capture::CameraCalibrationKey key { "camera-facetime", "MacBook Pro Speakers" };

    f.run (AppIntent { SaveCameraCalibration { key.cameraId, key.outputDevice, 35.0 } });

    const auto stored = overdub::capture::findCameraCalibration (f.files.cameraCalibration, key);
    REQUIRE (stored.has_value());
    CHECK (*stored == std::optional<double> (35.0));
    CHECK (f.calibrationPanel.clapResultLabel.getText().toStdString().starts_with ("Saved: 35.0 ms (video late) for MacBook Pro Speakers."));
    CHECK_FALSE (overdub::ui::testing::isShown (f.calibrationPanel.saveCameraButton));
}

TEST_CASE ("opening a Song, changing the device and calibrating are refused while a Pass runs", "[ui][workspace][record]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().record().has_value());
    overdub::core::ScopedLogCapture capture;

    CHECK_FALSE (f.workspace->idleOrNotice());
    f.run (AppIntent { applyCurrent (f) });
    f.run (AppIntent { MeasureLatency {} });

    CHECK (f.opened.size() == 1);
    CHECK (f.song().isRecording());
    CHECK (contains (messageCodes (capture), "ui.stop_recording_first"));
}

TEST_CASE ("opening another Song, changing the device and quitting are refused while a Separation runs, until it is cancelled", "[ui][workspace][separation]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    auto log = std::make_shared<overdub::song::testing::FakeHelperLog>();
    f.song().setSeparationTools (overdub::song::SeparationTools {
        .model = overdub::song::testing::writeStandInModel (f.folder),
        .startHelper = overdub::song::testing::fakeSeparationHelper ({}, log),
        .workFolder = f.folder.getChildFile ("work"),
    });
    REQUIRE (f.song().startSeparation (f.song().song().tracks.front().id).has_value());
    overdub::core::ScopedLogCapture capture;

    CHECK_FALSE (f.workspace->isIdle());
    CHECK_FALSE (f.workspace->idleOrNotice());
    const auto applied = f.run (AppIntent { applyCurrent (f) });
    REQUIRE_FALSE (applied.has_value());
    CHECK (applied.error().code == "ui.separation_running");
    CHECK_FALSE (f.run (AppIntent { Quit {} }).has_value());

    CHECK (f.opened.size() == 1);
    CHECK (f.song().isSeparating());
    CHECK (f.quits == 0);
    const auto codes = messageCodes (capture);
    CHECK (std::ranges::count (codes, std::string ("ui.separation_running")) == 3);

    f.song().cancelSeparation();
    CHECK (f.workspace->isIdle());
    CHECK (f.run (AppIntent { Quit {} }).has_value());
    CHECK (f.quits == 1);
}

//==============================================================================
// Sample rate conversion (issue #160)

namespace
{
// Polls as the window's timers would until the conversion has ended.
void finishConversion (WorkspaceFixture& f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (f.song().isConverting() && std::chrono::steady_clock::now() < deadline)
    {
        f.tick();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    REQUIRE_FALSE (f.song().isConverting());
    f.tick(); // the Workspace answers the rate change
}
} // namespace

TEST_CASE ("the mismatch Notice offers to convert the Song to the device's rate; converted, it plays and records on that device", "[ui][workspace][rate][conversion]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong (44100); // the device runs at 48 kHz only
    overdub::core::ScopedLogCapture capture;

    auto& action = f.content.noticeLine.actionButton;
    REQUIRE (isShown (action));
    CHECK (action.getButtonText() == "Convert to 48000 Hz");
    click (action);
    REQUIRE (f.song().isConverting());
    CHECK (isShown (f.content.runDialog));
    CHECK (f.content.runDialog.title.getText() == "Converting to 48000 Hz");
    CHECK_FALSE (isShown (action));

    finishConversion (f);
    CHECK_FALSE (isShown (f.content.runDialog));
    CHECK (f.song().song().sampleRate == 48000);
    CHECK (f.song().package().undoLabel() == "Convert to 48000 Hz");
    REQUIRE (f.opened.size() == 2);
    CHECK (f.openedForRates.back() == 48000);
    CHECK_FALSE (f.song().audioProblem().has_value());
    CHECK (f.song().audio().engine == f.audio().engine());

    REQUIRE (f.song().record().has_value());
    f.runBlocks (20);
    CHECK (f.song().isRecording());
    f.song().stop();
    f.runBlocks (1);
    CHECK_FALSE (f.song().isRecording());
    CHECK (f.song().song().passes.size() == 1);

    std::vector<overdub::core::Fields> reopened;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.audio_reopened")
            reopened.push_back (event);
    REQUIRE (reopened.size() == 1);
    CHECK (reopened[0].at ("change") == "song_rate_changed");
}

TEST_CASE ("undoing a conversion brings the Song back at its old rate, without audio on a device that does not offer it", "[ui][workspace][rate][conversion][history]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong (44100);
    f.content.run (AppIntent { ConvertSampleRate { 48000 } });
    finishConversion (f);
    REQUIRE (f.song().song().sampleRate == 48000);

    f.content.run (overdub::ui::SongIntent { Undo {} });
    f.tick();
    CHECK (f.song().song().sampleRate == 44100);
    REQUIRE (f.opened.size() == 3);
    REQUIRE (f.song().audioProblem().has_value());
    CHECK (f.song().audioProblem()->code == "ui.sample_rate_mismatch");

    f.content.run (overdub::ui::SongIntent { Redo {} });
    f.tick();
    CHECK (f.song().song().sampleRate == 48000);
    CHECK_FALSE (f.song().audioProblem().has_value());
}

TEST_CASE ("while a conversion runs, recording, other Songs and device changes wait; Cancel leaves the Song as it was", "[ui][workspace][conversion]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong (44100);
    const auto before = f.song().song();
    f.content.run (AppIntent { ConvertSampleRate { 48000 } });
    REQUIRE (f.song().isConverting());
    overdub::core::ScopedLogCapture capture;

    const auto recorded = f.song().record();
    REQUIRE_FALSE (recorded.has_value());
    CHECK (recorded.error().code == "ui.conversion_running");
    CHECK_FALSE (f.workspace->isIdle());
    CHECK_FALSE (f.workspace->idleOrNotice());
    CHECK (contains (messageCodes (capture), "ui.conversion_running"));

    click (f.content.runDialog.cancelButton);
    CHECK_FALSE (f.song().isConverting());
    CHECK_FALSE (isShown (f.content.runDialog));
    f.tick();
    CHECK (f.song().song() == before);
    CHECK (f.opened.size() == 1);
}

TEST_CASE ("a conversion is refused while a Pass runs", "[ui][workspace][conversion][record]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    REQUIRE (f.song().record().has_value());
    overdub::core::ScopedLogCapture capture;

    f.content.run (AppIntent { ConvertSampleRate { 44100 } });
    CHECK_FALSE (f.song().isConverting());
    CHECK (f.song().isRecording());
    CHECK (contains (messageCodes (capture), "ui.stop_recording_first"));
}

//==============================================================================
// The settings window's offer (issue #159)

TEST_CASE ("the settings window lists the rates of the devices being picked, only those both offer", "[ui][workspace][settings]")
{
    WorkspaceFixture f;
    f.machine.inputDevice = "MacBook Pro Microphone";
    f.machine.outputDevice = "MacBook Pro Speakers";
    f.machine.deviceRate = 44100;
    f.machine.offeredRates = { 44100, 48000 };
    f.machine.offersByDevice["NUX NGA-3BT"] = { { 48000 }, { 256 } };
    f.launch();
    f.devicePanel.show (f.audio().deviceChoices());
    REQUIRE (f.devicePanel.sampleRatePicker.getText() == "44100 Hz");
    std::optional<overdub::engine::DeviceSettings> applied;
    f.devicePanel.onApply = [&] (const overdub::engine::DeviceSettings& settings) { applied = settings; };

    choose (f.devicePanel.inputPicker, 1); // NUX NGA-3BT in, MacBook Pro Speakers out
    REQUIRE (f.devicePanel.inputPicker.getText() == "NUX NGA-3BT");
    REQUIRE (f.devicePanel.sampleRatePicker.getNumItems() == 1);
    CHECK (f.devicePanel.sampleRatePicker.getText() == "48000 Hz");
    CHECK (f.devicePanel.bufferSizePicker.getText() == "256 samples");

    click (f.devicePanel.applyButton);
    REQUIRE (applied.has_value());
    CHECK (applied->sampleRate == 48000);
}

TEST_CASE ("a picked rate that opens at another one says so", "[ui][workspace][settings][rate]")
{
    WorkspaceFixture f; // the device runs at 48 kHz whatever is picked
    f.launch();
    overdub::core::ScopedLogCapture capture;
    auto settings = f.audio().deviceChoices().current;
    settings.sampleRate = 44100;

    f.run (AppIntent { ApplyDeviceSettings { settings.deviceType, settings.inputDevice, settings.outputDevice, settings.sampleRate, settings.bufferSize } });

    std::vector<overdub::core::Fields> substituted;
    for (const auto& event : capture.events())
        if (event.at ("event") == "ui.device_rate_substituted")
            substituted.push_back (event);
    REQUIRE (substituted.size() == 1);
    CHECK (substituted[0].at ("requested_rate") == 44100);
    CHECK (substituted[0].at ("opened_rate") == 48000);
    CHECK (f.content.noticeLine.text() == "The audio device doesn't offer 44100 Hz, so it opened at 48000 Hz.");
}

TEST_CASE ("a picked rate that opens as picked, or at the open Song's rate, says nothing", "[ui][workspace][settings][rate]")
{
    WorkspaceFixture f;
    f.launch();
    f.openSong();
    overdub::core::ScopedLogCapture capture;
    f.run (AppIntent { applyCurrent (f) });
    CHECK_FALSE (contains (messageCodes (capture), "ui.device_rate_substituted"));
}
