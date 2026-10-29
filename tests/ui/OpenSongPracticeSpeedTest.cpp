#include "ui/OpenSong.h"

#include "../song/AudioMeasurements.h"
#include "core/Log.h"
#include "engine/EngineFixture.h"
#include "song/AtomicWrite.h"
#include "song/PracticeSpeed.h"
#include "song/SongJson.h"
#include "song/Transpose.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using overdub::ui::AudioBinding;
using overdub::ui::OpenSong;
using namespace overdub::engine::testing;

// Practice Speed in the open Song (issue #212, ADR 0009): not saved and not an undo step; renders
// of every source a Clip plays are made on a worker that poll() drives, into a temp folder outside
// the package, and playback switches to them at the same Song time once they are all there.
namespace
{
constexpr std::int64_t clickAt = 24000; // Song time 0.5 s

// An OpenSong bound to an engine, over a Song with a Backing Track "Click" (a 1 s impulse at 0.5 s,
// on its own Track) and a Take "take-a" of the same audio in a Clip at 0 on a Track "Guitar", so
// both are heard at the same Song time. A Take "take-b" (an impulse at 0.25 s) is in no Clip.
struct PracticeFixture
{
    TempPackage folder;
    juce::File root = folder.root.getChildFile ("Song.overdub");
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    std::unique_ptr<OpenSong> openSong;
    DeviceDriver device { 2, 256 };

    PracticeFixture()
    {
        const auto click = folder.root.getChildFile ("Click.wav");
        writeFloatWav (click, { impulse (48000, clickAt) });
        auto created = overdub::song::SongPackage::create (root, overdub::song::makeEmptySong (testSampleRate));
        REQUIRE (created.has_value());
        REQUIRE ((*created)->importBackingTrack (click).has_value());

        auto song = (*created)->song();
        for (const auto& [id, at] : { std::pair { "take-a", clickAt }, std::pair { "take-b", (std::int64_t) 12000 } })
        {
            overdub::song::Take take;
            take.id = id;
            take.file = std::string ("audio/") + id + ".wav";
            take.channels = 1;
            take.lengthSamples = 48000;
            writeFloatWav (root.getChildFile (take.file), { impulse (48000, at) });
            song.takes.push_back (take);
        }
        auto guitar = trackWith ("guitar", { clipOf ("clip-a", song.takes[0], 0, 0, 48000) });
        song.tracks.push_back (guitar);
        REQUIRE ((*created)->edit (song, "Add a Take").has_value());
        openSong = std::make_unique<OpenSong> (std::move (*created), AudioBinding { &engine, {}, {} });
    }

    ~PracticeFixture() { openSong.reset(); }

    const overdub::song::Song& song() const { return openSong->song(); }

    // Polls, as the window's timer does, until no Practice Speed render runs; returns every Error
    // poll() handed back.
    std::vector<overdub::core::Error> pollUntilRendered()
    {
        std::vector<overdub::core::Error> errors;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        do
        {
            for (auto& error : openSong->poll())
                errors.push_back (std::move (error));
            if (! openSong->isPracticeRendering())
                return errors;
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL ("the Practice Speed renders did not finish");
        return errors;
    }

    // Mutes every Track but `heardTrack` (an index into the Song's Tracks).
    void hearOnly (std::size_t heardTrack)
    {
        auto changed = song();
        for (std::size_t i = 0; i < changed.tracks.size(); ++i)
            changed.tracks[i].mute = i != heardTrack;
        REQUIRE (openSong->edit (changed, "Mute").has_value());
    }

    // Plays `seconds` from Song time 0 and returns the left output.
    std::vector<float> playFromStart (double seconds)
    {
        REQUIRE (openSong->seek (0).has_value());
        if (! openSong->isPlaying())
            REQUIRE (openSong->playOrStop().has_value());
        const auto left = device.render (engine, (int) (seconds * testSampleRate / 256)).channels[0];
        openSong->stop();
        device.render (engine, 1);
        return left;
    }

    int rendersInFolder() const { return openSong->practiceRenderFolder().findChildFiles (juce::File::findFiles, false).size(); }
};

std::int64_t peakIndex (const std::vector<float>& samples)
{
    std::int64_t peak = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (std::abs (samples[i]) > std::abs (samples[(std::size_t) peak]))
            peak = (std::int64_t) i;
    return peak;
}
} // namespace

TEST_CASE ("a Practice Speed change plays the previous audio until every source's render is ready", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    const auto historyBefore = f.openSong->package().undoLabel();

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());

    CHECK (f.openSong->practiceSpeed() == 80);
    CHECK (f.openSong->heardPracticeSpeed() == 100);
    CHECK (f.openSong->isPracticeRendering());
    // Not part of the Song, nor of its history.
    CHECK (f.openSong->package().undoLabel() == historyBefore);

    CHECK (f.pollUntilRendered().empty());
    CHECK (f.openSong->heardPracticeSpeed() == 80);
    CHECK (f.rendersInFolder() == 2);
    // Renders are temporary files outside the package.
    CHECK_FALSE (f.openSong->practiceRenderFolder().isAChildOf (f.root));
}

TEST_CASE ("a value outside 25..125 is refused and changes nothing", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;

    const auto refused = f.openSong->setPracticeSpeed (126);

    REQUIRE_FALSE (refused.has_value());
    CHECK (refused.error().code == "song.practice_speed_out_of_range");
    CHECK (f.openSong->practiceSpeed() == 100);
    CHECK_FALSE (f.openSong->isPracticeRendering());
}

TEST_CASE ("at 80% the playhead moves at 0.8 times real time, and a Take and a Backing Track that line up at 100% still line up", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    f.hearOnly (0);
    const auto backingAtFullSpeed = peakIndex (f.playFromStart (1.0));
    f.hearOnly (1);
    const auto takeAtFullSpeed = peakIndex (f.playFromStart (1.0));
    REQUIRE (backingAtFullSpeed == clickAt);
    REQUIRE (takeAtFullSpeed == clickAt);

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntilRendered();
    const auto take = peakIndex (f.playFromStart (1.0));
    f.hearOnly (0);
    REQUIRE (f.openSong->seek (0).has_value());
    REQUIRE (f.openSong->playOrStop().has_value());
    const auto backing = peakIndex (f.device.render (f.engine, 160).channels[0]);

    // ADR 0005: the impulse at Song time 0.5 s is heard within 1 ms of 0.5 s / 0.8.
    CHECK_THAT ((double) take, WithinAbs (clickAt / 0.8, 48.0));
    CHECK (backing == take);
    // 160 blocks of 256 samples move the playhead 40960 × 0.8 Song samples.
    CHECK (f.openSong->position() == 32768);
}

TEST_CASE ("at 25% a sine Backing Track keeps its frequency, and an impulse at Song time t is heard after 4t", "[ui][open_song][practice_speed]")
{
    // A Song with one Backing Track: a 440 Hz sine for 2 s with an impulse over it at Song time 0.5 s.
    TempPackage folder;
    const auto root = folder.root.getChildFile ("Song.overdub");
    auto samples = sine (2 * testSampleRate, 440.0);
    samples[(std::size_t) clickAt] = 1.0f;
    const auto wav = folder.root.getChildFile ("Sine.wav");
    writeFloatWav (wav, { samples });
    auto created = overdub::song::SongPackage::create (root, overdub::song::makeEmptySong (testSampleRate));
    REQUIRE (created.has_value());
    REQUIRE ((*created)->importBackingTrack (wav).has_value());
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    auto openSong = std::make_unique<OpenSong> (std::move (*created), AudioBinding { &engine, {}, {} });
    DeviceDriver device { 2, 256 };

    REQUIRE (openSong->setPracticeSpeed (25).has_value());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    while (openSong->isPracticeRendering() && std::chrono::steady_clock::now() < deadline)
    {
        CHECK (openSong->poll().empty());
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    REQUIRE (openSong->heardPracticeSpeed() == 25);
    REQUIRE (openSong->playOrStop().has_value());
    // 1400 blocks: about 7.5 s of playback, within the 8 s the 2 s Song lasts at 25%.
    const auto left = device.render (engine, 1400).channels[0];
    // 358400 samples of playback move the playhead 358400 × 0.25 Song samples.
    CHECK (openSong->position() == 89600);
    openSong->stop();
    device.render (engine, 1);

    // ADR 0005: within 5 cents, measured away from the impulse (heard at 2 s) and the ends.
    const auto measured = overdub::song::testing::measuredFrequency (left.data(), 3 * testSampleRate, 7 * testSampleRate, testSampleRate);
    CHECK_THAT (1200.0 * std::log2 (measured / 440.0), WithinAbs (0.0, 5.0));
    // ADR 0005: within 1 ms of 4 × 0.5 s.
    CHECK_THAT ((double) peakIndex (left), WithinAbs (4.0 * clickAt, 48.0));
    openSong.reset();
}

TEST_CASE ("a Practice Speed change during play switches to the renders at the same Song time", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    REQUIRE (f.openSong->playOrStop().has_value());
    f.device.render (f.engine, 4);

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.device.render (f.engine, 4);
    // The previous audio plays on at 100% while the renders are made.
    CHECK (f.openSong->position() == 2048);

    f.pollUntilRendered();
    const auto switchedAt = f.openSong->position();
    f.device.render (f.engine, 10);

    CHECK (f.openSong->heardPracticeSpeed() == 80);
    CHECK_THAT ((double) f.openSong->position(), WithinAbs ((double) switchedAt + 2560 * 0.8, 1.0));
}

TEST_CASE ("a source with no render yet plays silent while it is rendered", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntilRendered();

    auto changed = f.song();
    changed.tracks.push_back (trackWith ("keys", { clipOf ("clip-b", changed.takes[1], 0, 0, 48000) }));
    REQUIRE (f.openSong->edit (changed, "Add Keys").has_value());
    f.hearOnly (2);

    CHECK (f.openSong->isPracticeRendering());
    CHECK (nonZeroSamples (f.playFromStart (1.0)).empty());

    f.pollUntilRendered();
    CHECK_THAT ((double) peakIndex (f.playFromStart (1.0)), WithinAbs (12000 / 0.8, 48.0));
}

TEST_CASE ("back at 100% playback uses the Song's own audio at once and no render stays", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    REQUIRE (f.openSong->setPracticeSpeed (125).has_value());
    f.pollUntilRendered();
    REQUIRE (f.openSong->heardPracticeSpeed() == 125);

    REQUIRE (f.openSong->setPracticeSpeed (100).has_value());

    CHECK (f.openSong->heardPracticeSpeed() == 100);
    CHECK_FALSE (f.openSong->isPracticeRendering());
    CHECK (f.rendersInFolder() == 0);
    f.hearOnly (1);
    CHECK (nonZeroSamples (f.playFromStart (1.0)) == std::vector<std::int64_t> { clickAt });
}

TEST_CASE ("a Transpose change at a Practice Speed renders the Backing Tracks again at the new Transpose", "[ui][open_song][practice_speed][transpose]")
{
    PracticeFixture f;
    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntilRendered();
    const auto band = f.song().backingTracks.at (0);
    const overdub::song::ClipSource backingSource { overdub::song::ClipSourceKind::backing, band.id };

    auto changed = overdub::song::setTranspose (f.song(), 2);
    REQUIRE (changed.has_value());
    REQUIRE (f.openSong->edit (*changed, overdub::song::transposeLabel (2)).has_value());
    CHECK (f.openSong->isPracticeRendering());
    f.pollUntilRendered();
    while (f.openSong->isTransposing())
        f.openSong->poll();

    const auto folder = f.openSong->practiceRenderFolder();
    CHECK (overdub::song::practiceRenderFileFor (folder, backingSource, { .speedPercent = 80, .semitones = 2 }).existsAsFile());
    CHECK_FALSE (overdub::song::practiceRenderFileFor (folder, backingSource, { .speedPercent = 80, .semitones = 0 }).exists());
    CHECK (f.rendersInFolder() == 2);
}

TEST_CASE ("Practice Speed is not saved: the Song opens at 100%, and closing it discards the renders", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    f.pollUntilRendered();
    const auto folder = f.openSong->practiceRenderFolder();
    REQUIRE (folder.isDirectory());

    f.openSong.reset();

    CHECK_FALSE (folder.exists());
    CHECK_FALSE (f.root.getChildFile ("song.json").loadFileAsString().contains ("ractice"));
    auto reopened = overdub::song::SongPackage::open (f.root);
    REQUIRE (reopened.has_value());
    OpenSong openSong { std::move (reopened->package), AudioBinding { &f.engine, {}, {} } };
    CHECK (openSong.practiceSpeed() == 100);
    CHECK (openSong.heardPracticeSpeed() == 100);
}

TEST_CASE ("a failed render keeps the previous audio, hands its Error back once, and the same value again tries it again", "[ui][open_song][practice_speed]")
{
    PracticeFixture f;
    // A folder that is not empty where the Take's render's temp file would go: the write fails.
    const overdub::song::ClipSource takeA { overdub::song::ClipSourceKind::take, "take-a" };
    const auto blocker = overdub::song::tempFileFor (overdub::song::practiceRenderFileFor (f.openSong->practiceRenderFolder(), takeA, { .speedPercent = 80 }));
    REQUIRE (blocker.createDirectory().wasOk());
    REQUIRE (blocker.getChildFile ("in the way").replaceWithText ("x"));

    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    const auto errors = f.pollUntilRendered();

    REQUIRE (errors.size() == 1);
    CHECK (errors[0].code == "song.practice_render_failed");
    CHECK (errors[0].fields.at ("source_id") == "take-a");
    CHECK (f.openSong->heardPracticeSpeed() == 100);
    CHECK (f.openSong->poll().empty());
    CHECK_FALSE (f.openSong->isPracticeRendering());

    REQUIRE (blocker.deleteRecursively());
    REQUIRE (f.openSong->setPracticeSpeed (80).has_value());
    CHECK (f.openSong->isPracticeRendering());
    CHECK (f.pollUntilRendered().empty());
    CHECK (f.openSong->heardPracticeSpeed() == 80);
}
