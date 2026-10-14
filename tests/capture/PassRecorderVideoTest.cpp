#include "capture/PassRecorder.h"

#include "capture/FakeCamera.h"
#include "engine/EngineFixture.h"
#include "song/AtomicWrite.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using overdub::capture::PassPhase;
using overdub::capture::PassRecorder;
using overdub::capture::PassRecorderOptions;
using overdub::capture::RecordedPass;
using overdub::capture::RecordLatency;
using overdub::capture::testing::FakeCamera;
using overdub::capture::testing::MovieGate;
using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SampleCount;
using overdub::song::SongPackage;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 480; // 10 ms at 48 kHz: host times advance by exactly 10'000'000 ns
constexpr std::uint64_t firstBlockHostNs = 20'000'000'000ULL;

// A package with one Armed Track on input 0, set up as one undo step, an engine playing it, and a
// fake camera that is open.
struct VideoFixture
{
    TempPackage folder;
    std::unique_ptr<SongPackage> package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, blockSize };
    FakeCamera camera;

    VideoFixture()
    {
        auto created = SongPackage::create (folder.root.getChildFile ("Song.overdub"), overdub::song::makeEmptySong (testSampleRate));
        REQUIRE (created.has_value());
        package = std::move (*created);
        auto song = package->song();
        auto track = trackWith ("track-guitar", {});
        track.armed = true;
        track.inputChannels = { 0 };
        song.tracks.push_back (track);
        REQUIRE (package->edit (song, "Set up").has_value());
        engine.loadSong (package->song(), package->location());
        device.hostTimeNs = firstBlockHostNs;
        REQUIRE (camera.open ("camera-facetime").has_value());
    }

    // Plays from Song sample 0 and records `blocks` blocks with the camera, then stops and finishes.
    overdub::core::Expected<RecordedPass> record (RecordLatency latency, SampleCount calibrationSamples, int blocks)
    {
        PassRecorderOptions options;
        options.camera = &camera;
        options.cameraCalibrationSamples = calibrationSamples;
        options.outputDevice = "MacBook Pro Speakers";
        auto started = PassRecorder::start (*package, engine, latency, options);
        REQUIRE (started.has_value());
        engine.play();
        device.run (engine, blocks);
        (*started)->requestStop();
        device.run (engine, 1);
        REQUIRE ((*started)->phase() == PassPhase::saving);
        return (*started)->finish().pass;
    }
};

std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("a Pass with the camera records a Video Take placed by heard time", "[capture][record][video]")
{
    VideoFixture fixture;
    // The first frame was captured 1.5 s after the first block's output IO time.
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 1'500'000'000ULL;
    fixture.camera.movie.frames = 90;
    fixture.camera.movie.durationSeconds = 3.0;

    const auto pass = fixture.record (RecordLatency { 64, 96, 0 }, 48, 400);
    REQUIRE (pass.has_value());
    REQUIRE (pass->videoTakeId.has_value());
    CHECK (pass->notices.empty());

    const auto& song = fixture.package->song();
    REQUIRE (song.videoTakes.size() == 1);
    const auto& videoTake = song.videoTakes[0];
    CHECK (videoTake.id == *pass->videoTakeId);
    CHECK (videoTake.passId == pass->passId);
    CHECK (videoTake.file == "video/" + videoTake.id + ".mov");
    CHECK (videoTake.cameraId == "camera-facetime");
    // 1.5 s after Song sample 0 left the output IO, Song sample 72000 does; 96 samples of output
    // latency earlier is what is heard, and the camera's 48-sample calibration moves it earlier.
    CHECK (videoTake.startSample == 72'000 - 96 - 48);
    CHECK (videoTake.calibrationOffsetSamples == 48);
    // The output device the Pass played through, with the camera the calibration key (issue #68).
    CHECK (videoTake.outputDevice == "MacBook Pro Speakers");
    CHECK (videoTake.songSamplesPerMovieSecond == Catch::Approx ((double) testSampleRate).epsilon (1.0e-9));

    const auto movie = fixture.package->location().getChildFile (videoTake.file);
    CHECK (movie.existsAsFile());
    CHECK_FALSE (overdub::song::tempFileFor (movie).exists());
    REQUIRE (fixture.camera.moviesBegun.size() == 1);
    CHECK (fixture.camera.moviesBegun[0] == overdub::song::tempFileFor (movie));
}

TEST_CASE ("a Pass's Video Take joins the Pass's undo step", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 100'000'000ULL;
    fixture.camera.movie.frames = 10;
    REQUIRE (fixture.record (RecordLatency {}, 0, 50).has_value());
    CHECK (fixture.package->undoLabel() == "Record Pass 1");
    CHECK (fixture.package->song().videoTakes.size() == 1);

    REQUIRE (fixture.package->undo().has_value());
    CHECK (fixture.package->song().passes.empty());
    CHECK (fixture.package->song().takes.empty());
    CHECK (fixture.package->song().videoTakes.empty());

    REQUIRE (fixture.package->redo().has_value());
    CHECK (fixture.package->song().videoTakes.size() == 1);
}

TEST_CASE ("a Pass whose movie fails is kept without a Video Take and reports a Notice", "[capture][record][video]")
{
    ScopedLogCapture capture;
    VideoFixture fixture;
    fixture.camera.endError = overdub::core::Error { "capture.movie_no_frames", "the camera delivered no frames", {} };

    const auto pass = fixture.record (RecordLatency {}, 0, 50);
    REQUIRE (pass.has_value());
    CHECK_FALSE (pass->videoTakeId.has_value());
    REQUIRE (pass->notices.size() == 1);
    CHECK (pass->notices[0].code == "capture.movie_no_frames");
    CHECK (fixture.package->song().passes.size() == 1);
    CHECK (fixture.package->song().videoTakes.empty());
    // The half-written movie is not left in the package.
    CHECK_FALSE (fixture.camera.moviesBegun[0].exists());

    const auto stopped = eventsNamed (capture, "capture.video_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["outcome"] == "failed");
    // The Error itself is logged once, where the UI presents the Notice.
    CHECK (eventsNamed (capture, "capture.movie_no_frames").empty());
}

TEST_CASE ("a Pass whose movie cannot begin records audio only and reports a Notice", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.beginError = overdub::core::Error { "capture.movie_write_failed", "the movie could not be written", {} };

    const auto pass = fixture.record (RecordLatency {}, 0, 50);
    REQUIRE (pass.has_value());
    CHECK_FALSE (pass->videoTakeId.has_value());
    REQUIRE (pass->notices.size() == 1);
    CHECK (pass->notices[0].code == "capture.movie_write_failed");
    CHECK (fixture.camera.moviesEnded == 0);
    CHECK (fixture.package->song().takes.size() == 1);
}

TEST_CASE ("dropped camera frames keep the Video Take and report a Notice", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs;
    fixture.camera.movie.frames = 40;
    fixture.camera.movie.droppedFrames = 3;

    const auto pass = fixture.record (RecordLatency {}, 0, 50);
    REQUIRE (pass.has_value());
    CHECK (pass->videoTakeId.has_value());
    REQUIRE (pass->notices.size() == 1);
    CHECK (pass->notices[0].code == "capture.video_frames_dropped");
    CHECK (pass->notices[0].fields["dropped_frames"] == 3);
}

TEST_CASE ("a Pass with the camera logs its Video Take's start and stop", "[capture][record][video][log]")
{
    ScopedLogCapture capture;
    VideoFixture fixture;
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 1'000'000'000ULL;
    fixture.camera.movie.frames = 60;
    fixture.camera.movie.durationSeconds = 2.0;
    fixture.camera.movie.longestGapSeconds = 1.25;

    const auto pass = fixture.record (RecordLatency {}, 0, 300);
    REQUIRE (pass.has_value());

    const auto started = eventsNamed (capture, "capture.video_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0]["pass_id"] == pass->passId);
    CHECK (started[0]["video_take_id"] == *pass->videoTakeId);
    CHECK (started[0]["camera_id"] == "camera-facetime");

    const auto stopped = eventsNamed (capture, "capture.video_stopped");
    REQUIRE (stopped.size() == 1);
    CHECK (stopped[0]["outcome"] == "recorded");
    CHECK (stopped[0]["frames"] == 60);
    CHECK (stopped[0]["dropped_frames"] == 0);
    CHECK (stopped[0]["start_sample"] == 48'000);
    CHECK (stopped[0]["longest_gap_seconds"] == 1.25);
    CHECK (stopped[0]["anchors"] == 300);

    const auto passStarted = eventsNamed (capture, "capture.pass_started");
    REQUIRE (passStarted.size() == 1);
    CHECK (passStarted[0]["camera"] == true);
    const auto passStopped = eventsNamed (capture, "capture.pass_stopped");
    REQUIRE (passStopped.size() == 1);
    CHECK (passStopped[0]["camera"] == true);
    CHECK (passStopped[0]["video_take_id"] == *pass->videoTakeId);
}

TEST_CASE ("a Pass stopped before it began discards its movie", "[capture][record][video]")
{
    VideoFixture fixture;
    PassRecorderOptions options;
    options.camera = &fixture.camera;
    auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {}, options);
    REQUIRE (started.has_value());
    (*started)->requestStop();

    const auto finished = (*started)->poll (SongPackage::Clock::now());
    REQUIRE (finished.has_value());
    REQUIRE_FALSE (finished->pass.has_value());
    CHECK (finished->pass.error().code == "capture.pass_empty");
    CHECK (finished->stopReason == overdub::engine::RecordStopReason::cancelled);
    CHECK (fixture.camera.moviesCancelled == 1);
    CHECK (fixture.camera.moviesEnded == 0);
    CHECK_FALSE (fixture.camera.moviesBegun[0].exists());
    CHECK (fixture.package->song().videoTakes.empty());
}

TEST_CASE ("a Pass with the camera is saving until its movie completes, then poll returns it with its Video Take", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 100'000'000ULL;
    fixture.camera.movie.frames = 10;
    fixture.camera.finishGate = std::make_shared<MovieGate>();

    PassRecorderOptions options;
    options.camera = &fixture.camera;
    auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {}, options);
    REQUIRE (started.has_value());
    auto& recorder = **started;
    CHECK (recorder.phase() == PassPhase::waiting);
    fixture.engine.play();
    fixture.device.run (fixture.engine, 50);
    CHECK (recorder.phase() == PassPhase::capturing);
    recorder.requestStop();
    fixture.device.run (fixture.engine, 1);
    CHECK (recorder.phase() == PassPhase::saving);

    // Ending the movie does not wait for AVFoundation to complete it: well under a 60 Hz frame.
    const auto before = std::chrono::steady_clock::now();
    CHECK_FALSE (recorder.poll (SongPackage::Clock::now()).has_value());
    CHECK (std::chrono::steady_clock::now() - before < std::chrono::milliseconds (16));
    CHECK (fixture.camera.moviesEnded == 1);
    CHECK (recorder.phase() == PassPhase::saving);
    CHECK (fixture.package->song().passes.empty());

    fixture.camera.finishGate->release();
    std::optional<overdub::capture::FinishedPass> finished;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (! finished.has_value() && std::chrono::steady_clock::now() < deadline)
    {
        finished = recorder.poll (SongPackage::Clock::now());
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    REQUIRE (finished.has_value());
    CHECK (finished->stopReason == overdub::engine::RecordStopReason::requested);
    REQUIRE (finished->pass.has_value());
    REQUIRE (finished->pass->videoTakeId.has_value());
    CHECK (recorder.phase() == PassPhase::finished);
    CHECK_FALSE (recorder.poll (SongPackage::Clock::now()).has_value()); // returned once
    // The Pass, its Take and its Video Take are one undo step.
    CHECK (fixture.package->undoLabel() == "Record Pass 1");
    CHECK (fixture.package->song().passes.size() == 1);
    CHECK (fixture.package->song().videoTakes.size() == 1);
    REQUIRE (fixture.package->undo().has_value());
    CHECK (fixture.package->song().passes.empty());
    CHECK (fixture.package->song().videoTakes.empty());
}

TEST_CASE ("releasing a Pass whose movie still completes does not wait for it, and poll finishes it without the engine", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.movie.firstFrameHostNs = firstBlockHostNs + 100'000'000ULL;
    fixture.camera.movie.frames = 10;
    fixture.camera.finishGate = std::make_shared<MovieGate>();
    PassRecorderOptions options;
    options.camera = &fixture.camera;
    auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {}, options);
    REQUIRE (started.has_value());
    auto& recorder = **started;
    fixture.engine.play();
    fixture.device.run (fixture.engine, 50);

    // The device keeps calling back on another thread, as a live one would, until release() returns.
    std::atomic<bool> releasing { true };
    std::thread audio ([&] {
        while (releasing.load())
        {
            fixture.device.run (fixture.engine, 1);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    });
    const auto released = recorder.release (std::chrono::milliseconds (2000));
    releasing = false;
    audio.join();

    CHECK_FALSE (released.has_value()); // the movie is still completing
    CHECK (fixture.camera.moviesEnded == 1);
    CHECK (recorder.phase() == PassPhase::saving);

    fixture.camera.finishGate->release();
    std::optional<overdub::capture::FinishedPass> finished;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (! finished.has_value() && std::chrono::steady_clock::now() < deadline)
    {
        finished = recorder.poll (SongPackage::Clock::now());
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    REQUIRE (finished.has_value());
    REQUIRE (finished->pass.has_value());
    CHECK (finished->pass->videoTakeId.has_value());
}

TEST_CASE ("releasing a Pass the audio thread never ends returns capture.stop_timeout", "[capture][record]")
{
    VideoFixture fixture;
    auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {});
    REQUIRE (started.has_value());
    fixture.engine.play();
    fixture.device.run (fixture.engine, 10);

    const auto released = (*started)->release (std::chrono::milliseconds (20)); // no block runs again

    REQUIRE (released.has_value());
    REQUIRE_FALSE (released->pass.has_value());
    CHECK (released->pass.error().code == "capture.stop_timeout");
    fixture.device.run (fixture.engine, 1); // lets the destructor's wait end the Pass
}

TEST_CASE ("a Pass the device stopped during is returned with deviceStopped as its stop reason", "[capture][record]")
{
    VideoFixture fixture;
    auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {});
    REQUIRE (started.has_value());
    fixture.engine.play();
    fixture.device.run (fixture.engine, 10);

    fixture.engine.deviceStopped();
    const auto finished = (*started)->poll (SongPackage::Clock::now());

    REQUIRE (finished.has_value());
    CHECK (finished->stopReason == overdub::engine::RecordStopReason::deviceStopped);
    REQUIRE (finished->pass.has_value()); // keeps what it captured
    CHECK (finished->pass->lengthSamples == 10 * blockSize);
}

TEST_CASE ("a recorder destroyed while its movie completes deletes the movie", "[capture][record][video]")
{
    VideoFixture fixture;
    fixture.camera.movie.frames = 10;
    fixture.camera.finishGate = std::make_shared<MovieGate>();
    juce::File movieFile;
    {
        PassRecorderOptions options;
        options.camera = &fixture.camera;
        auto started = PassRecorder::start (*fixture.package, fixture.engine, RecordLatency {}, options);
        REQUIRE (started.has_value());
        fixture.engine.play();
        fixture.device.run (fixture.engine, 20);
        (*started)->requestStop();
        fixture.device.run (fixture.engine, 1);
        REQUIRE_FALSE ((*started)->poll (SongPackage::Clock::now()).has_value());
        movieFile = fixture.camera.moviesBegun.at (0);
        fixture.camera.finishGate->release();
    }
    CHECK_FALSE (movieFile.exists());
    CHECK (fixture.package->song().passes.empty());
}
