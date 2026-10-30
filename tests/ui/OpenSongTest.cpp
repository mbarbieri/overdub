#include "ui/OpenSong.h"

#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::song::testing;
using namespace overdub::engine::testing;

namespace
{
constexpr int blockSize = 256;

// A OpenSong over the 3-Track fixture, bound to an engine driven by a loopback fake device.
struct OpenSongFixture
{
    std::unique_ptr<FixtureSong> fixture = threeTrackFixture();
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    LoopbackDevice device { 2, blockSize, 512 };
    std::unique_ptr<OpenSong> openSong;

    OpenSongFixture()
    {
        openSong = std::make_unique<OpenSong> (std::move (fixture->package), AudioBinding { &engine, {}, { "In 1", "In 2" } });
    }

    ~OpenSongFixture()
    {
        openSong.reset(); // closes the package before the fixture deletes its folder
    }

    const overdub::song::Song& song() const { return openSong->song(); }

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

TEST_CASE ("a recorded Pass becomes one undo step once the stop reaches the audio thread", "[ui][open_song]")
{
    OpenSongFixture f;
    REQUIRE (f.openSong->record().has_value());
    CHECK (f.openSong->isRecording());
    CHECK (f.openSong->recordingPassNumber() == 1);

    f.device.run (f.engine, 20);
    CHECK (f.engine.isRecording());

    f.openSong->playOrStop();
    CHECK (f.openSong->isRecording()); // until the audio thread ends it and poll() finishes it
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());

    CHECK (! f.openSong->isRecording());
    CHECK (! f.engine.isPlaying());
    CHECK (f.openSong->package().undoLabel() == "Record Pass 1");
    CHECK (f.song().passes.size() == 1);
    CHECK (f.song().takes.size() == 2);
    CHECK (f.trackNamed ("Guitar").clips.size() == 1);
    CHECK (f.trackNamed ("Vocals").clips.size() == 1);
}

TEST_CASE ("R while a Pass records starts no second Pass", "[ui][open_song]")
{
    OpenSongFixture f;
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    REQUIRE (f.openSong->record().has_value());
    f.openSong->stop();
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());
    CHECK (f.song().passes.size() == 1);
}

TEST_CASE ("recording with nothing Armed, or without an audio device, is refused", "[ui][open_song]")
{
    SECTION ("nothing Armed")
    {
        OpenSongFixture f;
        f.openSong->setArmed (f.trackNamed ("Guitar").id, false);
        f.openSong->setArmed (f.trackNamed ("Vocals").id, false);
        const auto started = f.openSong->record();
        REQUIRE (! started.has_value());
        CHECK (started.error().code == "capture.nothing_armed");
        CHECK (! f.openSong->isRecording());
    }
    SECTION ("no device")
    {
        auto fixture = threeTrackFixture();
        OpenSong openSong { std::move (fixture->package), AudioBinding {} };
        const auto started = openSong.record();
        REQUIRE (! started.has_value());
        CHECK (started.error().code == "ui.no_audio_device");
    }
}

TEST_CASE ("arm, input and Monitoring change the Song without a history step", "[ui][open_song]")
{
    OpenSongFixture f;
    const auto stepBefore = f.openSong->package().undoLabel();
    const auto guitar = f.trackNamed ("Guitar").id;

    f.openSong->setArmed (guitar, false);
    f.openSong->setInputChannels (guitar, { 0, 1 });
    f.openSong->setMonitoring (guitar, true);

    CHECK (! f.trackNamed ("Guitar").settings.armed);
    CHECK (f.trackNamed ("Guitar").settings.inputChannels == std::vector<int> { 0, 1 });
    CHECK (f.openSong->isMonitoring (guitar));
    CHECK (f.openSong->package().undoLabel() == stepBefore);
    CHECK (f.openSong->package().hasPendingWrite());
}

TEST_CASE ("Monitoring lets an Armed Track's live input through to the outputs", "[ui][open_song]")
{
    OpenSongFixture f;
    const auto guitar = f.trackNamed ("Guitar").id;
    const auto vocals = f.trackNamed ("Vocals").id;
    f.openSong->setArmed (vocals, false);
    // One impulse on input 1 while Monitoring is off, one after it is on. The second one's echo
    // through the fake loop (512 samples later) comes after the last block.
    constexpr std::int64_t heardAt = 6 * blockSize + 10;
    f.device.extraInput[0] = impulse (8 * blockSize, 2 * blockSize);
    f.device.extraInput[0][(std::size_t) heardAt] = 1.0f;

    // Seek past the Backing Track's end so only live input can sound.
    f.openSong->setMonitoring (guitar, false);
    f.engine.seek (48000 * 10);
    f.device.run (f.engine, 4);
    f.openSong->setMonitoring (guitar, true);
    f.device.run (f.engine, 4);

    const auto heard = nonZeroSamples (f.device.outputHistory[0]);
    REQUIRE (heard.size() == 1);
    CHECK (heard[0] == heardAt);
}

TEST_CASE ("Undo and Redo step through the history and keep the engine on the current Song", "[ui][open_song]")
{
    OpenSongFixture f;
    REQUIRE (f.openSong->package().undoLabel() == "Add Tracks");
    REQUIRE (f.openSong->undo().has_value());
    CHECK (f.song().tracks.size() == 1);
    CHECK (f.openSong->package().redoLabel() == "Add Tracks");
    REQUIRE (f.openSong->redo().has_value());
    CHECK (f.song().tracks.size() == 3);
}

TEST_CASE ("an import adds one Track per file as its own undo step", "[ui][open_song]")
{
    OpenSongFixture f;
    const auto file = f.fixture->writeSineFile ("Drums.wav", 1.0, 110.0);
    const auto results = f.openSong->importFiles ({ file });
    REQUIRE (results.size() == 1);
    CHECK (results[0].has_value());
    CHECK (f.song().tracks.size() == 4);
    CHECK (f.song().tracks.back().name == "Drums");
    CHECK (f.openSong->package().undoLabel() == "Import Drums.wav");
}

TEST_CASE ("closing a Song while a Pass records finishes the Pass first", "[ui][open_song]")
{
    OpenSongFixture f;
    const auto location = f.openSong->package().location();
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 8);

    // The device keeps calling back while the openSong closes, as a live one would.
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
        f.openSong.reset();
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

TEST_CASE ("errors from finishing a Pass are handed back by poll", "[ui][open_song]")
{
    OpenSongFixture f;
    REQUIRE (f.openSong->record().has_value());
    f.openSong->stop(); // before the audio thread took the Pass: it is cancelled, nothing captured
    f.device.run (f.engine, 1);
    CHECK (errorCodes (f.openSong->poll()) == std::vector<std::string> { "capture.pass_empty" });
    CHECK (! f.openSong->isRecording());
}

TEST_CASE ("rewinding during a Pass ends it where it stopped and plays on from the start", "[ui][open_song]")
{
    OpenSongFixture f;
    f.engine.seek (48000);
    f.device.run (f.engine, 1);
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 8);

    f.openSong->rewind();
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());

    REQUIRE (f.song().passes.size() == 1);
    CHECK (f.song().passes[0].startSample == 48000);
    for (const auto& take : f.song().takes)
        CHECK (take.lengthSamples == 8 * blockSize);
    CHECK (f.openSong->isPlaying());
    CHECK (f.openSong->position() == blockSize); // one block played from sample 0
}

TEST_CASE ("an edit is one labelled undo step, and the engine plays the edited Song", "[ui][open_song][edit]")
{
    OpenSongFixture f;
    const auto before = f.song();
    auto changed = before;
    changed.tracks[0].clips.clear();

    REQUIRE (f.openSong->edit (changed, "Delete Clip").has_value());
    CHECK (f.song() == changed);
    CHECK (f.openSong->package().undoLabel() == "Delete Clip");

    // The Band Mix Clip is gone, so the engine renders silence.
    f.openSong->playOrStop();
    f.device.run (f.engine, 8);
    CHECK (nonZeroSamples (f.device.outputHistory[0]).empty());

    REQUIRE (f.openSong->undo().has_value());
    CHECK (f.song() == before);
}

TEST_CASE ("edits and seeks are refused while a Pass runs", "[ui][open_song][edit]")
{
    OpenSongFixture f;
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    const auto before = f.song();
    auto changed = before;
    changed.tracks[0].clips.clear();

    CHECK (f.openSong->edit (changed, "Delete Clip").error().code == "ui.stop_recording_first");
    CHECK (f.song() == before);
    const auto position = f.openSong->position();
    CHECK (f.openSong->seek (100000).error().code == "ui.stop_recording_first");
    CHECK (f.openSong->position() == position);

    f.openSong->stop();
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());
}

TEST_CASE ("seeking moves the playhead, which is saved once the transport is stopped", "[ui][open_song]")
{
    OpenSongFixture f;
    f.openSong->seek (12345);
    CHECK (f.openSong->position() == 12345);
    f.device.run (f.engine, 1);
    CHECK (f.openSong->poll().empty());
    CHECK (f.song().settings.playhead == 12345);

    f.openSong->seek (-10);
    CHECK (f.openSong->position() == 0);
}

TEST_CASE ("a Clip source's peaks come from the package's cache, and a missing source has none", "[ui][open_song]")
{
    OpenSongFixture f;
    const auto& source = f.trackNamed ("Band Mix").clips.at (0).source;
    const auto expected = f.openSong->package().peaks (source);
    REQUIRE (expected.has_value());

    CHECK (f.openSong->peaks (source) == *expected);
    CHECK (f.openSong->peaks (source).lengthSamples == 96000);

    ScopedLogCapture capture;
    const overdub::song::ClipSource unknown { overdub::song::ClipSourceKind::take, "no-such-take" };
    CHECK (f.openSong->peaks (unknown).channels.empty());
    CHECK (f.openSong->peaks (unknown).channels.empty());
    // Logged once, where the failure is handled; the second lookup uses the cached result.
    std::vector<std::string> events;
    for (const auto& event : capture.events())
        events.push_back (event.at ("event"));
    CHECK (events == std::vector<std::string> { "song.media_unknown" });
}

namespace
{
std::size_t plansBuilt (const ScopedLogCapture& capture)
{
    return (std::size_t) std::ranges::count_if (capture.events(), [] (const auto& event) { return event.at ("event") == "engine.plan_built"; });
}

// The peak of output channel 0 over the last `samples` samples the device rendered.
float recentPeak (const LoopbackDevice& device, std::size_t samples)
{
    const auto& output = device.outputHistory[0];
    float peak = 0.0f;
    for (auto i = output.size() - samples; i < output.size(); ++i)
        peak = std::max (peak, std::abs (output[i]));
    return peak;
}
} // namespace

TEST_CASE ("a Mixer edit and its undo and redo are heard without rebuilding the engine's plan", "[ui][open_song][mixer]")
{
    OpenSongFixture f;
    f.openSong->playOrStop();
    f.device.run (f.engine, 4);
    REQUIRE (recentPeak (f.device, blockSize) > 0.0f);

    ScopedLogCapture capture;
    const auto bandMix = f.trackNamed ("Band Mix").id;
    auto muted = f.song();
    muted.tracks[0].mute = true;
    REQUIRE (f.openSong->edit (muted, "Mute Band Mix").has_value());
    f.device.run (f.engine, 2); // one block ramps down, then silence
    CHECK_THAT (recentPeak (f.device, blockSize), Catch::Matchers::WithinAbs (0.0, 0.0));

    REQUIRE (f.openSong->undo().has_value());
    f.device.run (f.engine, 2);
    CHECK (recentPeak (f.device, blockSize) > 0.0f);
    REQUIRE (f.openSong->redo().has_value());
    f.device.run (f.engine, 2);
    CHECK_THAT (recentPeak (f.device, blockSize), Catch::Matchers::WithinAbs (0.0, 0.0));
    CHECK (plansBuilt (capture) == 0);

    // Any other change still rebuilds the plan.
    auto renamed = f.song();
    renamed.tracks[0].name = "Band";
    REQUIRE (f.openSong->edit (renamed, "Rename").has_value());
    CHECK (plansBuilt (capture) == 1);
    CHECK (f.song().tracks[0].id == bandMix);
}

TEST_CASE ("a Mixer preview is heard but leaves the Song and its history alone", "[ui][open_song][mixer]")
{
    OpenSongFixture f;
    f.openSong->playOrStop();
    const auto before = f.song();
    const auto stepsBefore = f.openSong->package().undoLabel();

    auto quieter = before;
    quieter.master.volumeDb = -60.0;
    f.openSong->previewMix (quieter);
    f.device.run (f.engine, 4);
    const auto quietPeak = recentPeak (f.device, blockSize);

    f.openSong->previewMix (before);
    f.device.run (f.engine, 4);
    const auto fullPeak = recentPeak (f.device, blockSize);

    CHECK (f.song() == before);
    CHECK (f.openSong->package().undoLabel() == stepsBefore);
    CHECK (quietPeak > 0.0f);
    CHECK (quietPeak < fullPeak / 500.0f); // -60 dB is 1/1000
}

TEST_CASE ("a Pass that starts during a Mixer preview plays the Song's own mix", "[ui][open_song][mixer]")
{
    OpenSongFixture f;
    auto quieter = f.song();
    quieter.master.volumeDb = -60.0;
    f.openSong->previewMix (quieter);
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 4);
    const auto recordingPeak = recentPeak (f.device, blockSize);
    f.openSong->stop();
    f.device.run (f.engine, 1);
    f.openSong->poll();

    // The same span played again at the Song's own mix (Master at 0 dB) peaks the same.
    f.openSong->seek (0);
    f.openSong->playOrStop();
    f.device.run (f.engine, 4);
    CHECK (recordingPeak > 0.5f * recentPeak (f.device, blockSize));
}

TEST_CASE ("the meters report what each Track and the Master played since the last poll", "[ui][open_song][mixer][meters]")
{
    OpenSongFixture f;
    f.openSong->playOrStop();
    f.device.run (f.engine, 8);

    const auto peaks = f.openSong->meterPeaks();
    CHECK (peaks.tracks.at (f.trackNamed ("Band Mix").id) > 0.0f);
    CHECK_THAT (peaks.tracks.at (f.trackNamed ("Guitar").id), Catch::Matchers::WithinAbs (0.0, 0.0));
    CHECK (peaks.master > 0.0f);
    CHECK_THAT (f.openSong->meterPeaks().master, Catch::Matchers::WithinAbs (0.0, 0.0)); // nothing played since
}

TEST_CASE ("a new latency calibration places the next Pass's Takes without rebinding the engine", "[ui][open_song][calibration]")
{
    OpenSongFixture f;
    f.openSong->setCalibrationOffset (140);
    CHECK (f.openSong->audio().latency.calibrationOffsetSamples == 140);

    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 10);
    f.openSong->playOrStop();
    f.device.run (f.engine, 1);
    REQUIRE (f.openSong->poll().empty());

    REQUIRE (f.song().takes.size() == 2);
    for (const auto& take : f.song().takes)
    {
        CHECK (take.compensation.calibrationOffsetSamples == 140);
        CHECK (take.origin == -140);
    }
}
