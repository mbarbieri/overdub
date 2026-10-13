#include "ui/OpenSong.h"

#include "capture/FakeCamera.h"
#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"
#include "ui/UserMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

// Device changes (issue #41, ADR 0004 amendment): the audio device that stops during a Pass, a
// device at another rate than the Song's, and cameras plugged in or out.
using overdub::capture::RecordLatency;
using overdub::capture::testing::FakeCamera;
using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using overdub::ui::Tier;
using overdub::ui::tierFor;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 480; // 10 ms at 48 kHz
constexpr std::uint64_t firstBlockHostNs = 7'000'000'000ULL;

// A 48 kHz Song with "Guitar" Armed on input 0 (and the camera Armed on "camera-usb" when asked),
// opened on a loopback-driven engine, with a fake camera attached.
struct DeviceChangeFixture
{
    TempPackage folder;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, blockSize };
    FakeCamera camera;
    std::unique_ptr<OpenSong> openSong;

    explicit DeviceChangeFixture (bool cameraArmed = false, int deviceSampleRate = testSampleRate)
    {
        auto song = overdub::song::makeEmptySong (testSampleRate);
        auto guitar = trackWith ("guitar", {});
        guitar.armed = true;
        guitar.inputChannels = { 0 };
        song.tracks = { guitar };
        song.cameraArmed = cameraArmed;
        song.cameraDeviceId = cameraArmed ? "camera-usb" : "";
        auto created = SongPackage::create (folder.root.getChildFile ("Song.overdub"), song);
        REQUIRE (created.has_value());
        openSong = std::make_unique<OpenSong> (std::move (*created), binding (deviceSampleRate));
        REQUIRE (openSong->attachCamera (&camera, folder.root.getChildFile ("camera-calibration.json")).has_value());
        device.hostTimeNs = firstBlockHostNs;
        camera.movie.firstFrameHostNs = firstBlockHostNs + 100'000'000ULL;
        camera.movie.frames = 30;
    }

    ~DeviceChangeFixture()
    {
        if (openSong != nullptr && openSong->isRecording())
        {
            openSong->stop();
            device.run (engine, 1);
            openSong->poll();
        }
        openSong.reset();
    }

    AudioBinding binding (int deviceSampleRate)
    {
        return AudioBinding { &engine, RecordLatency { 0, 120, 0 }, { "In 1", "In 2" }, "Speakers", deviceSampleRate };
    }

    void unplugCamera (const std::string& cameraId)
    {
        std::erase_if (camera.available, [&] (const auto& listed) { return listed.id == cameraId; });
        camera.devicesChanged = true;
    }
};

std::vector<std::string> errorCodes (const std::vector<overdub::core::Error>& errors)
{
    std::vector<std::string> codes;
    for (const auto& error : errors)
        codes.push_back (error.code);
    return codes;
}
} // namespace

//==============================================================================
// The audio device stops during a Pass

TEST_CASE ("a Pass the audio device stopped during is kept up to where it stopped, with an Alert", "[ui][open_song][device_change]")
{
    DeviceChangeFixture f;
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 10);

    f.engine.deviceStopped(); // the interface was unplugged: no callback runs again
    const auto errors = f.openSong->poll();
    REQUIRE (errorCodes (errors) == std::vector<std::string> { "ui.audio_device_lost_during_pass" });
    CHECK (errors[0].fields["pass_number"] == 1);
    CHECK (errors[0].fields["kept"] == true);
    CHECK (tierFor (errors[0].code) == Tier::alert);

    CHECK_FALSE (f.openSong->isRecording());
    CHECK_FALSE (f.openSong->isPlaying());
    const auto& song = f.openSong->song();
    REQUIRE (song.passes.size() == 1);
    REQUIRE (song.takes.size() == 1);
    CHECK (song.takes[0].lengthSamples == 10 * blockSize);
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
}

TEST_CASE ("a Pass that had not begun when the audio device stopped adds nothing, with the same Alert", "[ui][open_song][device_change]")
{
    DeviceChangeFixture f;
    REQUIRE (f.openSong->record().has_value());

    f.engine.deviceStopped();
    const auto errors = f.openSong->poll();
    REQUIRE (errorCodes (errors) == std::vector<std::string> { "ui.audio_device_lost_during_pass" });
    CHECK (errors[0].fields["kept"] == false);
    CHECK (f.openSong->song().passes.empty());
    CHECK_FALSE (f.openSong->isRecording());
}

TEST_CASE ("detaching the audio after the device stopped finishes the Pass at once, keeping it", "[ui][open_song][device_change]")
{
    DeviceChangeFixture f;
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 6);
    f.engine.deviceStopped();

    // The window rebuilds the audio before its next poll: no wait for a callback that never comes.
    const auto errors = f.openSong->detachAudio();
    CHECK (errorCodes (errors) == std::vector<std::string> { "ui.audio_device_lost_during_pass" });
    REQUIRE (f.openSong->song().passes.size() == 1);
    CHECK (f.openSong->song().takes[0].lengthSamples == 6 * blockSize);
}

TEST_CASE ("a Pass the audio device stopped during keeps its Video Take", "[ui][open_song][device_change][camera]")
{
    DeviceChangeFixture f { true };
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 20);
    f.engine.deviceStopped();

    CHECK (errorCodes (f.openSong->poll()) == std::vector<std::string> { "ui.audio_device_lost_during_pass" });
    const auto& song = f.openSong->song();
    REQUIRE (song.passes.size() == 1);
    REQUIRE (song.videoTakes.size() == 1);
    CHECK (song.videoTakes[0].passId == song.passes[0].id);
    CHECK (song.cameraArmed); // the camera did not go anywhere
}

TEST_CASE ("the audio device stopping while idle stops playback and says nothing", "[ui][open_song][device_change]")
{
    DeviceChangeFixture f;
    f.openSong->playOrStop();
    f.device.run (f.engine, 4);
    REQUIRE (f.openSong->isPlaying());

    f.engine.deviceStopped();
    CHECK (f.openSong->poll().empty());
    CHECK_FALSE (f.openSong->isPlaying());
    CHECK (f.openSong->position() == 4 * blockSize);
}

//==============================================================================
// A device at another rate than the Song's

TEST_CASE ("a device at another rate than the Song's is not used: the Song opens without audio and says why", "[ui][open_song][device_change][sample_rate]")
{
    DeviceChangeFixture f { false, 44100 };
    const auto problem = f.openSong->audioProblem();
    REQUIRE (problem.has_value());
    CHECK (problem->code == "ui.sample_rate_mismatch");
    CHECK (problem->fields["song_rate"] == testSampleRate);
    CHECK (problem->fields["device_rate"] == 44100);
    CHECK (tierFor (problem->code) == Tier::notice);

    // Neither recording nor playback runs at the wrong speed.
    const auto started = f.openSong->record();
    REQUIRE_FALSE (started.has_value());
    CHECK (started.error().code == "ui.sample_rate_mismatch");
    f.openSong->playOrStop();
    f.device.run (f.engine, 2);
    CHECK_FALSE (f.engine.isPlaying());
    CHECK (f.openSong->audio().engine == nullptr);

    // The device at the Song's rate (reopened by the app) is used again.
    CHECK (f.openSong->detachAudio().empty());
    f.openSong->attachAudio (f.binding (testSampleRate));
    CHECK_FALSE (f.openSong->audioProblem().has_value());
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    CHECK (f.engine.isRecording());
}

TEST_CASE ("without a device the audio problem is that none is open; a device that reports no rate is taken at the Song's", "[ui][open_song][device_change][sample_rate]")
{
    DeviceChangeFixture f { false, 0 };
    CHECK_FALSE (f.openSong->audioProblem().has_value());

    CHECK (f.openSong->detachAudio().empty());
    f.openSong->attachAudio (AudioBinding {});
    const auto problem = f.openSong->audioProblem();
    REQUIRE (problem.has_value());
    CHECK (problem->code == "ui.no_audio_device");
}

//==============================================================================
// Cameras plugged in or out

TEST_CASE ("a camera plugged in while the Song is open joins the camera choices", "[ui][open_song][device_change][camera]")
{
    ScopedLogCapture capture;
    DeviceChangeFixture f;
    REQUIRE (f.openSong->cameraDevices().size() == 2);

    f.camera.available.push_back ({ "camera-desk", "Desk Camera" });
    CHECK (f.openSong->cameraDevices().size() == 2); // until the camera reports the change
    f.camera.devicesChanged = true;
    CHECK (f.openSong->poll().empty());
    REQUIRE (f.openSong->cameraDevices().size() == 3);
    CHECK (f.openSong->cameraDevices()[2].id == "camera-desk");

    int changes = 0;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == "ui.cameras_changed")
        {
            ++changes;
            CHECK (event["cameras"] == 3);
        }
    CHECK (changes == 1);
}

TEST_CASE ("the Armed camera unplugged while idle is closed and disarmed, with a Notice; the choice stays", "[ui][open_song][device_change][camera]")
{
    DeviceChangeFixture f { true };
    REQUIRE (f.camera.opened.has_value());

    f.unplugCamera ("camera-usb");
    const auto errors = f.openSong->poll();
    REQUIRE (errorCodes (errors) == std::vector<std::string> { "ui.camera_disconnected" });
    CHECK (errors[0].fields["camera_id"] == "camera-usb");
    CHECK (errors[0].fields["during_pass"] == false);
    CHECK (tierFor (errors[0].code) == Tier::notice);

    CHECK_FALSE (f.camera.opened.has_value());
    CHECK_FALSE (f.openSong->song().cameraArmed);
    CHECK (f.openSong->song().cameraDeviceId == "camera-usb");
    CHECK (f.openSong->cameraDevices().size() == 1);
    CHECK_FALSE (f.openSong->package().canUndo()); // outside history, like arming
}

TEST_CASE ("an unplugged camera that is not Armed only leaves the choices", "[ui][open_song][device_change][camera]")
{
    DeviceChangeFixture f;
    f.unplugCamera ("camera-usb");
    CHECK (f.openSong->poll().empty());
    CHECK (f.openSong->cameraDevices().size() == 1);
    CHECK_FALSE (f.openSong->song().cameraArmed);
}

TEST_CASE ("the Armed camera unplugged during a Pass: recording goes on, the Pass keeps its Video Take, and the camera is disarmed after", "[ui][open_song][device_change][camera]")
{
    DeviceChangeFixture f { true };
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 10);

    f.unplugCamera ("camera-usb");
    const auto during = f.openSong->poll();
    REQUIRE (errorCodes (during) == std::vector<std::string> { "ui.camera_disconnected" });
    CHECK (during[0].fields["during_pass"] == true);
    CHECK (f.openSong->isRecording());
    CHECK (f.camera.writing.has_value()); // the movie is not cut off: it ends with the Pass

    f.device.run (f.engine, 10);
    f.openSong->stop();
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());

    const auto& song = f.openSong->song();
    REQUIRE (song.passes.size() == 1);
    CHECK (song.takes[0].lengthSamples == 20 * blockSize);
    REQUIRE (song.videoTakes.size() == 1);
    CHECK_FALSE (song.cameraArmed);
    CHECK_FALSE (f.camera.opened.has_value());
}
