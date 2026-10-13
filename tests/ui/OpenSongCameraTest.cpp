#include "ui/OpenSong.h"

#include "capture/CameraCalibration.h"
#include "capture/FakeCamera.h"
#include "engine/EngineFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

using overdub::capture::RecordLatency;
using overdub::capture::testing::FakeCamera;
using overdub::capture::testing::MovieGate;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 480; // 10 ms at 48 kHz
constexpr std::uint64_t firstBlockHostNs = 7'000'000'000ULL;

// A Song with "Guitar" Armed on input 0, opened through OpenSong on a loopback-driven engine
// whose output device is "Speakers" (output latency 120), with a fake camera attached.
struct CameraFixture
{
    TempPackage folder;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, blockSize };
    FakeCamera camera;
    juce::File calibrationFile = folder.root.getChildFile ("camera-calibration.json");
    std::unique_ptr<OpenSong> openSong;

    explicit CameraFixture (bool cameraArmed = false)
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
        openSong = std::make_unique<OpenSong> (std::move (*created), AudioBinding { &engine, RecordLatency { 0, 120, 0 }, { "In 1", "In 2" }, "Speakers" });
        device.hostTimeNs = firstBlockHostNs;
    }

    ~CameraFixture()
    {
        if (openSong != nullptr && openSong->isRecording())
        {
            openSong->stop();
            device.run (engine, 1);
            openSong->poll();
        }
        openSong.reset();
    }

    // Records `blocks` blocks from Song sample 0, stops, and finishes the Pass.
    std::vector<overdub::core::Error> recordPass (int blocks)
    {
        REQUIRE (openSong->record().has_value());
        device.run (engine, blocks);
        openSong->stop();
        device.run (engine, 1);
        return openSong->poll();
    }
};
} // namespace

TEST_CASE ("arming the camera opens the default camera, which becomes the Song's choice", "[ui][camera]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    CHECK_FALSE (fixture.camera.opened.has_value());
    CHECK (fixture.openSong->cameraDevices().size() == 2);

    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    CHECK (fixture.openSong->song().cameraArmed);
    REQUIRE (fixture.camera.opened.has_value());
    CHECK (fixture.openSong->song().cameraDeviceId == "camera-facetime");
    // Arming is outside history.
    CHECK_FALSE (fixture.openSong->package().canUndo());

    REQUIRE (fixture.openSong->setCameraArmed (false).has_value());
    CHECK_FALSE (fixture.openSong->song().cameraArmed);
    CHECK_FALSE (fixture.camera.opened.has_value());
}

TEST_CASE ("a Song opened with its camera Armed opens its chosen camera, and closing the Song closes it", "[ui][camera]")
{
    CameraFixture fixture { true };
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.camera.opened.has_value());
    CHECK (fixture.camera.opened->id == "camera-usb");

    fixture.openSong.reset();
    CHECK_FALSE (fixture.camera.opened.has_value());
}

TEST_CASE ("a chosen camera that is gone falls back to the default one, and says so in the log", "[ui][camera]")
{
    overdub::core::ScopedLogCapture capture;
    CameraFixture fixture { true };
    fixture.camera.available.pop_back(); // the USB camera was unplugged
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.camera.opened.has_value());
    CHECK (fixture.camera.opened->id == "camera-facetime");
    CHECK (fixture.openSong->song().cameraDeviceId == "camera-facetime");

    int fallbacks = 0;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == "ui.camera_fallback")
        {
            ++fallbacks;
            CHECK (event["chosen_camera_id"] == "camera-usb");
            CHECK (event["opened_camera_id"] == "camera-facetime");
        }
    CHECK (fallbacks == 1);
}

TEST_CASE ("choosing another camera reopens it while Armed", "[ui][camera]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.openSong->setCameraDevice ("camera-usb").has_value());
    CHECK (fixture.openSong->song().cameraDeviceId == "camera-usb");
    CHECK_FALSE (fixture.camera.opened.has_value()); // not Armed: stays closed

    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    REQUIRE (fixture.openSong->setCameraDevice ("camera-facetime").has_value());
    REQUIRE (fixture.camera.opened.has_value());
    CHECK (fixture.camera.opened->id == "camera-facetime");
}

TEST_CASE ("a camera that cannot open leaves the camera disarmed and says why", "[ui][camera]")
{
    CameraFixture fixture;
    fixture.camera.openError = overdub::core::Error { "capture.camera_access_denied", "the app may not use the camera", {} };
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());

    const auto armed = fixture.openSong->setCameraArmed (true);
    REQUIRE_FALSE (armed.has_value());
    CHECK (armed.error().code == "capture.camera_access_denied");
    CHECK_FALSE (fixture.openSong->song().cameraArmed);
}

TEST_CASE ("with the camera Armed each Pass records a Video Take, placed with this machine's calibration", "[ui][camera][record]")
{
    CameraFixture fixture;
    // The FaceTime camera on these speakers stamps frames 5 ms (240 samples) late.
    REQUIRE (overdub::capture::writeCameraCalibration (fixture.calibrationFile, { "camera-facetime", "Speakers" }, 5.0).has_value());
    REQUIRE (overdub::capture::writeCameraCalibration (fixture.calibrationFile, { "camera-facetime", "Headphones" }, 50.0).has_value());
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 500'000'000ULL;
    fixture.camera.movie.frames = 30;

    CHECK (fixture.recordPass (100).empty());
    const auto& song = fixture.openSong->song();
    REQUIRE (song.videoTakes.size() == 1);
    CHECK (song.videoTakes[0].passId == song.passes[0].id);
    // 0.5 s in: Song sample 24000 at the output IO, 120 samples heard earlier, 240 for the camera.
    CHECK (song.videoTakes[0].startSample == 24'000 - 120 - 240);
    CHECK (song.videoTakes[0].calibrationOffsetSamples == 240);
    // The Video Take keeps the output device of that calibration key (issue #68).
    CHECK (song.videoTakes[0].outputDevice == "Speakers");
}

TEST_CASE ("without the camera Armed a Pass records no Video Take", "[ui][camera][record]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    CHECK (fixture.recordPass (20).empty());
    CHECK (fixture.camera.moviesBegun.empty());
    CHECK (fixture.openSong->song().videoTakes.empty());
}

TEST_CASE ("a Pass whose movie failed is kept and reports the movie's Error", "[ui][camera][record]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    fixture.camera.endError = overdub::core::Error { "capture.movie_no_frames", "the camera delivered no frames", {} };

    const auto errors = fixture.recordPass (20);
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].code == "capture.movie_no_frames");
    CHECK (fixture.openSong->song().passes.size() == 1);
}

TEST_CASE ("the camera arm and choice are ignored while a Pass runs", "[ui][camera][record]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.openSong->record().has_value());
    fixture.device.run (fixture.engine, 2);

    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    REQUIRE (fixture.openSong->setCameraDevice ("camera-usb").has_value());
    CHECK_FALSE (fixture.openSong->song().cameraArmed);
    CHECK (fixture.openSong->song().cameraDeviceId.empty());
}

TEST_CASE ("the window does not wait while a slow movie completes; the Pass is added once it has", "[ui][camera][record]")
{
    CameraFixture fixture;
    REQUIRE (fixture.openSong->attachCamera (&fixture.camera, fixture.calibrationFile).has_value());
    REQUIRE (fixture.openSong->setCameraArmed (true).has_value());
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 100'000'000ULL;
    fixture.camera.movie.frames = 10;
    fixture.camera.finishGate = std::make_shared<MovieGate>();

    REQUIRE (fixture.openSong->record().has_value());
    fixture.device.run (fixture.engine, 50);
    fixture.openSong->stop();
    fixture.device.run (fixture.engine, 1);

    // No message-thread wait longer than a 60 Hz frame while the movie completes.
    constexpr auto oneFrame = std::chrono::microseconds (16'667);
    for (int poll = 0; poll < 3; ++poll)
    {
        const auto before = std::chrono::steady_clock::now();
        CHECK (fixture.openSong->poll().empty());
        CHECK (std::chrono::steady_clock::now() - before < oneFrame);
    }
    CHECK (fixture.camera.moviesEnded == 1);
    // The Pass is not in the Song yet, and the Song stays as the Pass started.
    CHECK (fixture.openSong->isRecording());
    CHECK (fixture.openSong->song().passes.empty());
    CHECK_FALSE (fixture.openSong->package().canUndo());

    fixture.camera.finishGate->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    // The finishing poll writes the Takes and song.json, as without a camera: not timed here.
    while (fixture.openSong->isRecording() && std::chrono::steady_clock::now() < deadline)
    {
        CHECK (fixture.openSong->poll().empty());
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    REQUIRE_FALSE (fixture.openSong->isRecording());

    // One undo step holds the Pass, its Take and its Video Take.
    const auto& song = fixture.openSong->song();
    CHECK (song.passes.size() == 1);
    REQUIRE (song.videoTakes.size() == 1);
    CHECK (song.videoTakes[0].passId == song.passes[0].id);
    CHECK (fixture.openSong->package().undoLabel() == "Record Pass 1");
    REQUIRE (fixture.openSong->undo().has_value());
    CHECK (fixture.openSong->song().passes.empty());
    CHECK (fixture.openSong->song().videoTakes.empty());
}
