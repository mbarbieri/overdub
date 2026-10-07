#include "ui/SongSession.h"

#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::SongSession;
using namespace overdub::song::testing;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 256;

// A SongSession over the 3-Track fixture, bound to an engine driven by a loopback fake device.
struct SessionFixture
{
    std::unique_ptr<FixtureSong> fixture = threeTrackFixture();
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };
    std::unique_ptr<SongSession> session;

    SessionFixture()
    {
        session = std::make_unique<SongSession> (std::move (fixture->package), AudioBinding { &engine, {}, { "In 1", "In 2" } });
    }

    ~SessionFixture()
    {
        session.reset(); // closes the package before the fixture deletes its folder
    }

    const overdub::song::Song& song() const { return session->song(); }

    const overdub::song::Track& trackNamed (const std::string& name) const
    {
        const auto found = std::ranges::find (song().tracks, name, &overdub::song::Track::name);
        REQUIRE (found != song().tracks.end());
        return *found;
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

TEST_CASE ("a recorded Pass becomes one undo step once the stop reaches the audio thread", "[ui][session]")
{
    SessionFixture f;
    REQUIRE (f.session->record().has_value());
    CHECK (f.session->isRecording());
    CHECK (f.session->recordingPassNumber() == 1);

    f.device.run (f.engine, 20);
    CHECK (f.engine.isRecording());

    f.session->playOrStop();
    CHECK (f.session->isRecording()); // until the audio thread ends it and poll() finishes it
    f.device.run (f.engine, 1);
    CHECK (f.session->poll().empty());

    CHECK (! f.session->isRecording());
    CHECK (! f.engine.isPlaying());
    CHECK (f.session->package().undoLabel() == "Record Pass 1");
    CHECK (f.song().passes.size() == 1);
    CHECK (f.song().takes.size() == 2);
    CHECK (f.trackNamed ("Guitar").clips.size() == 1);
    CHECK (f.trackNamed ("Vocals").clips.size() == 1);
}

TEST_CASE ("R while a Pass records starts no second Pass", "[ui][session]")
{
    SessionFixture f;
    REQUIRE (f.session->record().has_value());
    f.device.run (f.engine, 4);
    REQUIRE (f.session->record().has_value());
    f.session->stop();
    f.device.run (f.engine, 1);
    CHECK (f.session->poll().empty());
    CHECK (f.song().passes.size() == 1);
}

TEST_CASE ("recording with nothing Armed, or without an audio device, is refused", "[ui][session]")
{
    SECTION ("nothing Armed")
    {
        SessionFixture f;
        f.session->setArmed (f.trackNamed ("Guitar").id, false);
        f.session->setArmed (f.trackNamed ("Vocals").id, false);
        const auto started = f.session->record();
        REQUIRE (! started.has_value());
        CHECK (started.error().code == "capture.nothing_armed");
        CHECK (! f.session->isRecording());
    }
    SECTION ("no device")
    {
        auto fixture = threeTrackFixture();
        SongSession session { std::move (fixture->package), AudioBinding {} };
        const auto started = session.record();
        REQUIRE (! started.has_value());
        CHECK (started.error().code == "ui.no_audio_device");
    }
}

TEST_CASE ("arm, input and Monitoring change the Song without a history step", "[ui][session]")
{
    SessionFixture f;
    const auto stepBefore = f.session->package().undoLabel();
    const auto guitar = f.trackNamed ("Guitar").id;

    f.session->setArmed (guitar, false);
    f.session->setInputChannels (guitar, { 0, 1 });
    f.session->setMonitoring (guitar, true);

    CHECK (! f.trackNamed ("Guitar").armed);
    CHECK (f.trackNamed ("Guitar").inputChannels == std::vector<int> { 0, 1 });
    CHECK (f.session->isMonitoring (guitar));
    CHECK (f.session->package().undoLabel() == stepBefore);
    CHECK (f.session->package().hasPendingWrite());
}

TEST_CASE ("Monitoring lets an Armed Track's live input through to the outputs", "[ui][session]")
{
    SessionFixture f;
    const auto guitar = f.trackNamed ("Guitar").id;
    const auto vocals = f.trackNamed ("Vocals").id;
    f.session->setArmed (vocals, false);
    // One impulse on input 1 while Monitoring is off, one after it is on. The second one's echo
    // through the fake loop (512 samples later) comes after the last block.
    constexpr std::int64_t heardAt = 6 * blockSize + 10;
    f.device.extraInput[0] = impulse (8 * blockSize, 2 * blockSize);
    f.device.extraInput[0][(std::size_t) heardAt] = 1.0f;

    // Seek past the Backing Track's end so only live input can sound.
    f.session->setMonitoring (guitar, false);
    f.engine.seek (48000 * 10);
    f.device.run (f.engine, 4);
    f.session->setMonitoring (guitar, true);
    f.device.run (f.engine, 4);

    const auto heard = nonZeroSamples (f.device.outputHistory[0]);
    REQUIRE (heard.size() == 1);
    CHECK (heard[0] == heardAt);
}

TEST_CASE ("Undo and Redo step through the history and keep the engine on the current Song", "[ui][session]")
{
    SessionFixture f;
    REQUIRE (f.session->package().undoLabel() == "Add Tracks");
    REQUIRE (f.session->undo().has_value());
    CHECK (f.song().tracks.size() == 1);
    CHECK (f.session->package().redoLabel() == "Add Tracks");
    REQUIRE (f.session->redo().has_value());
    CHECK (f.song().tracks.size() == 3);
}

TEST_CASE ("an import adds one Track per file as its own undo step", "[ui][session]")
{
    SessionFixture f;
    const auto file = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    const auto results = f.session->importFiles ({ file });
    REQUIRE (results.size() == 1);
    CHECK (results[0].has_value());
    CHECK (f.song().tracks.size() == 4);
    CHECK (f.song().tracks.back().name == "Drums");
    CHECK (f.session->package().undoLabel() == "Import Drums.wav");
}

TEST_CASE ("closing a Song while a Pass records finishes the Pass first", "[ui][session]")
{
    SessionFixture f;
    const auto location = f.session->package().location();
    REQUIRE (f.session->record().has_value());
    f.device.run (f.engine, 8);

    // The device keeps calling back while the session closes, as a live one would.
    std::atomic<bool> running { true };
    std::thread audio { [&] {
        while (running.load())
        {
            f.device.processOneBlock (f.engine);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    } };
    {
        ScopedLogCapture capture;
        f.session.reset();
        running.store (false);
        audio.join();

        const auto events = capture.events();
        CHECK (std::ranges::any_of (events, [] (const auto& event) { return event.at ("event") == "capture.pass_stopped" && event.at ("outcome") == "recorded"; }));
    }

    auto reopened = SongPackage::open (location);
    REQUIRE (reopened.has_value());
    CHECK (reopened->package->song().passes.size() == 1);
    CHECK (reopened->package->undoLabel() == "Record Pass 1");
}

TEST_CASE ("errors from finishing a Pass are handed back by poll", "[ui][session]")
{
    SessionFixture f;
    REQUIRE (f.session->record().has_value());
    f.session->stop(); // before the audio thread took the Pass: it is cancelled, nothing captured
    f.device.run (f.engine, 1);
    CHECK (errorCodes (f.session->poll()) == std::vector<std::string> { "capture.pass_empty" });
    CHECK (! f.session->isRecording());
}
