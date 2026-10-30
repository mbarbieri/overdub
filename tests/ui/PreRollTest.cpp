#include "ui/OpenSong.h"

#include "song/AudioFileDecode.h"
#include "song/SongJson.h"

#include "engine/EngineFixture.h"
#include "song/FixtureSongBuilder.h"
#include "ui/TrackRowEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using overdub::capture::RecordLatency;
using overdub::core::ScopedLogCapture;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::song::PreRoll;
using overdub::song::SampleCount;
using overdub::song::SongPackage;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::engine::testing;

namespace
{
// 224 does not divide 2 s at 48 kHz: the record point falls inside a block.
constexpr int blockSize = 224;
constexpr SampleCount preRollSamples = 2 * testSampleRate;

// A package whose "Click" Track plays impulses at the given Song samples from a Take, and whose
// "Guitar" Track is Armed on input 0, opened through OpenSong on an engine that a loopback device
// (512 samples round trip, reported as 200 + 312) drives in blocks of 224.
struct PreRollFixture
{
    TempPackage folder;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    LoopbackDevice device { 2, blockSize, 512 };
    std::unique_ptr<OpenSong> openSong;

    explicit PreRollFixture (const std::vector<SampleCount>& clicks)
    {
        auto song = overdub::song::makeEmptySong (testSampleRate);
        auto created = SongPackage::create (folder.root.getChildFile ("Song.overdub"), song);
        REQUIRE (created.has_value());

        std::vector<float> samples ((std::size_t) (10 * testSampleRate), 0.0f);
        for (const auto click : clicks)
            samples[(std::size_t) click] = 1.0f;
        overdub::song::Take take;
        take.id = "click-take";
        take.file = "audio/click-take.wav";
        take.lengthSamples = (SampleCount) samples.size();
        writeFloatWav ((*created)->location().getChildFile (take.file), { samples });
        song.takes.push_back (take);
        auto click = trackWith ("click", { clipOf ("click-clip", take, 0, 0, take.lengthSamples) });
        click.name = "Click";
        auto guitar = trackWith ("guitar", {});
        guitar.name = "Guitar";
        guitar.settings.armed = true;
        guitar.settings.inputChannels = { 0 };
        song.tracks = { click, guitar };
        REQUIRE ((*created)->edit (song, "Set up").has_value());

        openSong = std::make_unique<OpenSong> (std::move (*created), AudioBinding { &engine, RecordLatency { 200, 312, 0 }, { "In 1", "In 2" } });
    }

    // A Pass still running is ended through the device first: with no callback, closing would wait
    // for the audio thread in vain.
    ~PreRollFixture()
    {
        if (openSong->isRecording())
        {
            openSong->stop();
            device.run (engine, 1);
            openSong->poll();
        }
        openSong.reset();
    }

    const overdub::song::Song& song() const { return openSong->song(); }

    const overdub::song::Take& guitarTake() const
    {
        const auto found = std::ranges::find (song().takes, "guitar", &overdub::song::Take::trackId);
        REQUIRE (found != song().takes.end());
        return *found;
    }

    std::vector<float> takeSamples (const overdub::song::Take& take) const
    {
        auto audio = overdub::song::readWavFile (openSong->package().location().getChildFile (take.file));
        REQUIRE (audio.has_value());
        return { audio->getReadPointer (0), audio->getReadPointer (0) + audio->getNumSamples() };
    }

    // Records with pre-roll from `recordPoint` for `blocks` blocks, then stops and finishes the Pass.
    void recordFrom (SampleCount recordPoint, int blocks)
    {
        openSong->changeSettings (overdub::ui::withPreRoll (openSong->song(), PreRoll::twoSeconds));
        openSong->seek (recordPoint);
        REQUIRE (openSong->record().has_value());
        device.run (engine, blocks);
        openSong->stop();
        device.run (engine, 1);
        REQUIRE (openSong->poll().empty());
        REQUIRE (song().passes.size() == 1);
    }
};
} // namespace

TEST_CASE ("with pre-roll 2 s, R plays from 2 s before the record point and the Take starts exactly there", "[ui][open_song][pre_roll][placement]")
{
    constexpr SampleCount recordPoint = 3 * testSampleRate;
    constexpr SampleCount playFrom = recordPoint - preRollSamples;
    constexpr SampleCount clickBefore = playFrom + 1000;    // heard in the pre-roll, never recorded
    constexpr SampleCount clickAfter = recordPoint + 3000;  // looped back into the Take
    PreRollFixture f { { clickBefore, clickAfter } };

    f.openSong->changeSettings (overdub::ui::withPreRoll (f.openSong->song(), PreRoll::twoSeconds));
    f.openSong->seek (recordPoint);
    REQUIRE (f.openSong->record().has_value());

    // During the pre-roll the Pass waits for its record point.
    f.device.run (f.engine, 100);
    CHECK (f.openSong->isRecording());
    CHECK (f.openSong->preRollRecordPoint() == recordPoint);
    CHECK_FALSE (f.openSong->runningPassStart().has_value());
    CHECK_FALSE (f.engine.isRecording());

    constexpr int blocks = 460; // past the second click's round trip
    f.device.run (f.engine, blocks - 100);
    CHECK_FALSE (f.openSong->preRollRecordPoint().has_value());
    CHECK (f.openSong->runningPassStart() == recordPoint);
    f.openSong->stop();
    f.device.run (f.engine, 1);
    REQUIRE (f.openSong->poll().empty());

    // Output started 2 s before the record point: stream position n played Song sample playFrom + n.
    CHECK (nonZeroSamples (f.device.outputHistory[0]) == std::vector<std::int64_t> { clickBefore - playFrom, clickAfter - playFrom });

    // The Pass and its Take start at the record point; only what came after it was written.
    REQUIRE (f.song().passes.size() == 1);
    CHECK (f.song().passes[0].startSample == recordPoint);
    const auto& take = f.guitarTake();
    CHECK (take.lengthSamples == (SampleCount) blocks * blockSize - preRollSamples);
    CHECK (take.origin == recordPoint - 512);
    const auto samples = f.takeSamples (take);
    REQUIRE (nonZeroSamples (samples).size() == 1);

    // Placement stays sample-exact: the looped-back click is heard where it was played.
    const auto clickInTake = nonZeroSamples (samples)[0];
    const auto& clip = f.song().tracks[1].clips.back();
    CHECK (clip.timelineStart == recordPoint);
    CHECK (clip.source.id == take.id);
    CHECK (clip.timelineStart + (clickInTake - clip.sourceOffset) == clickAfter);
}

TEST_CASE ("pre-roll near the Song start plays from 0, and the Pass still starts at the record point", "[ui][open_song][pre_roll]")
{
    constexpr SampleCount recordPoint = testSampleRate / 2;
    PreRollFixture f { {} };
    f.recordFrom (recordPoint, 200);

    CHECK (f.song().passes[0].startSample == recordPoint);
    CHECK (f.guitarTake().lengthSamples == 200 * blockSize - recordPoint);
}

TEST_CASE ("without pre-roll, or with the transport already playing, R records from the playhead at once", "[ui][open_song][pre_roll]")
{
    PreRollFixture f { {} };
    SECTION ("pre-roll off")
    {
        f.openSong->seek (testSampleRate);
        REQUIRE (f.openSong->record().has_value());
    }
    SECTION ("already playing")
    {
        f.openSong->changeSettings (overdub::ui::withPreRoll (f.openSong->song(), PreRoll::fourSeconds));
        f.openSong->seek (testSampleRate);
        f.openSong->playOrStop();
        f.device.run (f.engine, 1);
        REQUIRE (f.openSong->record().has_value());
    }
    CHECK_FALSE (f.openSong->preRollRecordPoint().has_value());
    f.device.run (f.engine, 1);
    CHECK (f.openSong->runningPassStart().has_value());
    CHECK (f.engine.isRecording());
}

TEST_CASE ("stopping during the pre-roll ends the Pass with nothing recorded", "[ui][open_song][pre_roll]")
{
    PreRollFixture f { {} };
    f.openSong->changeSettings (overdub::ui::withPreRoll (f.openSong->song(), PreRoll::oneSecond));
    f.openSong->seek (2 * testSampleRate);
    ScopedLogCapture capture;
    REQUIRE (f.openSong->record().has_value());
    f.device.run (f.engine, 20);
    f.openSong->stop();
    f.device.run (f.engine, 1);

    const auto errors = f.openSong->poll();
    REQUIRE (errors.size() == 1);
    CHECK (errors[0].code == "capture.pass_empty");
    CHECK (f.song().passes.empty());
    CHECK_FALSE (f.openSong->isRecording());

    // Logged as a pre-roll that never reached its record point: no capture.pass_started.
    const auto events = capture.events();
    const auto preRoll = std::ranges::find_if (events, [] (const auto& event) { return event.at ("event") == "ui.pre_roll_started"; });
    REQUIRE (preRoll != events.end());
    CHECK (preRoll->at ("record_point") == 2 * testSampleRate);
    CHECK (preRoll->at ("play_from") == testSampleRate);
    CHECK_FALSE (std::ranges::any_of (events, [] (const auto& event) { return event.at ("event") == "capture.pass_started"; }));
    CHECK (std::ranges::any_of (events, [] (const auto& event) { return event.at ("event") == "capture.pass_stopped" && event.at ("outcome") == "empty"; }));
}

TEST_CASE ("changing pre-roll writes the Song without a history step", "[ui][open_song][pre_roll]")
{
    PreRollFixture f { {} };
    const auto stepBefore = f.openSong->package().undoLabel();

    f.openSong->changeSettings (overdub::ui::withPreRoll (f.openSong->song(), PreRoll::fourSeconds));
    CHECK (f.song().settings.preRoll == PreRoll::fourSeconds);
    CHECK (f.openSong->package().undoLabel() == stepBefore);
    CHECK (f.openSong->package().hasPendingWrite());

    // The debounced write puts it in song.json.
    REQUIRE (f.openSong->poll (SongPackage::Clock::now() + std::chrono::seconds (5)).empty());
    const auto json = overdub::song::parseJsonText (f.openSong->package().location().getChildFile ("song.json").loadFileAsString().toStdString());
    REQUIRE (json.has_value());
    const auto written = overdub::song::songFromJson (*json);
    REQUIRE (written.has_value());
    CHECK (written->settings.preRoll == PreRoll::fourSeconds);
}
