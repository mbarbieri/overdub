#include "engine/Engine.h"
#include "song/BackingTrackImport.h"
#include "song/SongPackage.h"

#include "EngineFixture.h"
#include "TestProcessors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>

using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;

namespace
{
EngineConfig stereoConfig (int maxBlockSize = 256)
{
    return EngineConfig { .numOutputChannels = 2, .maxBlockSize = maxBlockSize };
}

std::vector<float> slice (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    return { samples.begin() + from, samples.begin() + to };
}
} // namespace

TEST_CASE ("an impulse in a Clip comes out at its timeline position", "[engine][playback]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    // Source impulse at 300; the Clip starts reading the Take at 100 and sits at timeline 1000,
    // so the impulse belongs at song sample 1000 + (300 - 100) = 1200.
    const auto take = addTake (song, package, "take-a", impulse (2000, 300));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 1000, 100, 1500) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 12);

    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { 1200 });
    CHECK (nonZeroSamples (rendered.channels[1]) == std::vector<std::int64_t> { 1200 });
    CHECK_THAT (rendered.channels[0][1200], Catch::Matchers::WithinULP (centrePanGain, 0));
}

TEST_CASE ("after a seek the impulse comes out where the new position puts it", "[engine][transport]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", impulse (2000, 300));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 1000, 100, 1500) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    engine.play();

    DeviceDriver device { 2, 256 };
    device.render (engine, 2);
    CHECK (engine.position() == 512);

    // Seek mid-playback to 1100: the impulse (song sample 1200) is 100 samples into the next block.
    engine.seek (1100);
    CHECK (engine.position() == 1100);
    const auto afterSeek = device.render (engine, 4);

    CHECK (nonZeroSamples (afterSeek.channels[0]) == std::vector<std::int64_t> { 100 });
    CHECK (nonZeroSamples (afterSeek.channels[1]) == std::vector<std::int64_t> { 100 });
    CHECK (engine.position() == 1100 + 4 * 256);
}

TEST_CASE ("after a seek under PDC the impulse comes out at its Song position and only silence precedes it", "[engine][transport][pdc]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    // The dry Track holds a constant level over Song samples 0..999, so the delay lines are full
    // of audio when the seek happens. The delayed Track has an impulse at Song sample 1600.
    const auto takeLevel = addTake (song, package, "take-level", std::vector<float> (1000, 0.25f));
    const auto takeImpulse = addTake (song, package, "take-impulse", impulse (4000, 1600));
    song.tracks.push_back (trackWith ("track-dry", { clipOf ("clip-level", takeLevel, 0, 0, 1000) }));
    song.tracks.push_back (trackWith ("track-delayed", { clipOf ("clip-impulse", takeImpulse, 0, 0, 4000) }));

    static constexpr int delaySamples = 128;
    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, insertOnTrack ("track-delayed", [] { return std::make_unique<DelayProcessor> (delaySamples); }));
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto beforeSeek = device.render (engine, 2);
    REQUIRE_FALSE (nonZeroSamples (beforeSeek.channels[0]).empty());

    // Seek to 1400: Song sample 1600 is output sample 200 after the seek. The first 128 output
    // samples would hold pre-seek audio from the delay lines; the engine silences them.
    engine.seek (1400);
    const auto afterSeek = device.render (engine, 4);

    CHECK (engine.latencySamples() == delaySamples);
    CHECK (nonZeroSamples (afterSeek.channels[0]) == std::vector<std::int64_t> { 200 });
    CHECK (nonZeroSamples (afterSeek.channels[1]) == std::vector<std::int64_t> { 200 });
    CHECK_THAT (afterSeek.channels[0][200], Catch::Matchers::WithinULP (centrePanGain, 0));
}

TEST_CASE ("a stopped transport outputs silence and holds its position", "[engine][transport]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto take = addTake (song, package, "take-a", impulse (2000, 0));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", take, 0, 0, 2000) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);

    DeviceDriver device { 2, 256 };
    const auto stopped = device.render (engine, 2);
    CHECK (nonZeroSamples (stopped.channels[0]).empty());
    CHECK (engine.position() == 0);
    CHECK_FALSE (engine.isPlaying());

    engine.play();
    const auto playing = device.render (engine, 1);
    CHECK (engine.isPlaying());
    CHECK (nonZeroSamples (playing.channels[0]) == std::vector<std::int64_t> { 0 });

    engine.stop();
    device.render (engine, 3);
    CHECK_FALSE (engine.isPlaying());
    CHECK (engine.position() == 256);
}

TEST_CASE ("where Clips overlap only the top Clip sounds", "[engine][playback]")
{
    TempPackage package;
    auto layered = overdub::song::makeEmptySong (testSampleRate);
    const auto bottomTake = addTake (layered, package, "take-bottom", sine (6000, 441.0));
    const auto topTake = addTake (layered, package, "take-top", sine (6000, 1234.5, 0.25f));
    // Bottom spans 0..5000; top covers 1500..3500 and is last, so it is on top.
    const auto bottom = clipOf ("clip-bottom", bottomTake, 0, 0, 5000);
    const auto top = clipOf ("clip-top", topTake, 1500, 700, 2000);

    auto topAlone = layered;
    auto bottomAlone = layered;
    layered.tracks.push_back (trackWith ("track", { bottom, top }));
    topAlone.tracks.push_back (trackWith ("track", { top }));
    bottomAlone.tracks.push_back (trackWith ("track", { bottom }));

    const auto renderSong = [&] (const overdub::song::Song& song) {
        Engine engine { stereoConfig() };
        engine.loadSong (song, package.root);
        engine.play();
        DeviceDriver device { 2, 256 };
        return device.render (engine, 24).channels[0];
    };

    const auto mixed = renderSong (layered);
    const auto onlyTop = renderSong (topAlone);
    const auto onlyBottom = renderSong (bottomAlone);

    CHECK (peakResidualDbfs (slice (mixed, 1500, 3500), slice (onlyTop, 1500, 3500)) <= -120.0);
    CHECK (peakResidualDbfs (slice (mixed, 0, 1500), slice (onlyBottom, 0, 1500)) <= -120.0);
    CHECK (peakResidualDbfs (slice (mixed, 3500, 5000), slice (onlyBottom, 3500, 5000)) <= -120.0);
    CHECK (nonZeroSamples (slice (mixed, 5000, 6144)).empty());
    // Sanity: the regions differ, so the null tests above can fail.
    CHECK (peakResidualDbfs (slice (onlyTop, 1500, 3500), slice (onlyBottom, 1500, 3500)) > -20.0);
}

TEST_CASE ("a Track with a delaying processor stays aligned with an undelayed Track", "[engine][pdc]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto takeA = addTake (song, package, "take-a", impulse (4000, 1200));
    const auto takeB = addTake (song, package, "take-b", impulse (4000, 2000));
    song.tracks.push_back (trackWith ("track-dry", { clipOf ("clip-a", takeA, 0, 0, 4000) }));
    song.tracks.push_back (trackWith ("track-delayed", { clipOf ("clip-b", takeB, 0, 0, 4000) }));

    static constexpr int delaySamples = 100;
    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root, insertOnTrack ("track-delayed", [] { return std::make_unique<DelayProcessor> (delaySamples); }));
    engine.play();

    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 16);

    CHECK (engine.latencySamples() == delaySamples);
    // Both impulses at their Song positions: PDC delays the dry Track by the same 100 samples and
    // the transport renders 100 samples ahead, so output sample n is Song sample n.
    CHECK (nonZeroSamples (rendered.channels[0]) == std::vector<std::int64_t> { 1200, 2000 });
    CHECK (nonZeroSamples (rendered.channels[1]) == std::vector<std::int64_t> { 1200, 2000 });
}

TEST_CASE ("an Insert Chain shapes only its own Track", "[engine][pdc]")
{
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto takeA = addTake (song, package, "take-a", impulse (2000, 500));
    const auto takeB = addTake (song, package, "take-b", impulse (2000, 900));
    song.tracks.push_back (trackWith ("track-a", { clipOf ("clip-a", takeA, 0, 0, 2000) }));
    song.tracks.push_back (trackWith ("track-b", { clipOf ("clip-b", takeB, 0, 0, 2000) }));

    const auto renderWith = [&] (const overdub::engine::InsertFactory& inserts) {
        Engine engine { stereoConfig() };
        engine.loadSong (song, package.root, inserts);
        engine.play();
        DeviceDriver device { 2, 256 };
        return device.render (engine, 8).channels[0];
    };

    const auto halved = renderWith (insertOnTrack ("track-b", [] { return std::make_unique<GainProcessor> (0.5f); }));
    CHECK_THAT (halved[500], Catch::Matchers::WithinULP (centrePanGain, 0));
    CHECK_THAT (halved[900], Catch::Matchers::WithinULP (0.5f * centrePanGain, 0));

    const auto silenced = renderWith (insertOnTrack ("track-a", [] { return std::make_unique<SilenceProcessor>(); }));
    CHECK (nonZeroSamples (silenced) == std::vector<std::int64_t> { 900 });
}

TEST_CASE ("a Clip whose source file is missing plays silence and still hides the Clips below", "[engine][playback]")
{
    overdub::core::ScopedLogCapture capture;
    TempPackage package;
    auto song = overdub::song::makeEmptySong (testSampleRate);
    const auto bottomTake = addTake (song, package, "take-bottom", impulse (2000, 600));
    const auto missingTake = addTake (song, package, "take-missing", impulse (2000, 0));
    package.root.getChildFile (missingTake.file).deleteFile();
    song.tracks.push_back (trackWith ("track", { clipOf ("clip-bottom", bottomTake, 0, 0, 2000),
                                                 clipOf ("clip-missing", missingTake, 500, 0, 200) }));

    Engine engine { stereoConfig() };
    engine.loadSong (song, package.root);
    engine.play();
    DeviceDriver device { 2, 256 };
    const auto rendered = device.render (engine, 8);

    CHECK (nonZeroSamples (rendered.channels[0]).empty());

    const auto events = capture.events();
    const auto missing = std::ranges::find_if (events, [] (const auto& e) { return e["event"] == "engine.clip_source_missing"; });
    REQUIRE (missing != events.end());
    CHECK ((*missing)["lvl"] == "warn");
    CHECK ((*missing)["source_kind"] == "take");
    CHECK ((*missing)["source_id"] == "take-missing");
    CHECK ((*missing)["path"] == package.root.getChildFile (missingTake.file).getFullPathName().toStdString());
}

TEST_CASE ("an imported Backing Track plays from sample 0, and plays silence once both its files are gone", "[engine][playback][import]")
{
    TempPackage folder;
    const auto source = folder.root.getChildFile ("Click.wav");
    writeFloatWav (source, { impulse (2000, 300) });
    const auto location = folder.root.getChildFile ("Song.overdub");
    {
        auto created = overdub::song::SongPackage::create (location, overdub::song::makeEmptySong (testSampleRate));
        REQUIRE (created.has_value());
        REQUIRE ((*created)->importBackingTrack (source).has_value());
    }
    auto opened = overdub::song::SongPackage::open (location);
    REQUIRE (opened.has_value());
    const auto song = opened->package->song();

    Engine engine { stereoConfig() };
    engine.loadSong (song, location);
    engine.play();
    DeviceDriver device { 2, 256 };
    CHECK (nonZeroSamples (device.render (engine, 4).channels[0]) == std::vector<std::int64_t> { 300 });

    const auto& backingTrack = song.backingTracks.at (0);
    REQUIRE (overdub::song::derivedFileFor (location, backingTrack).deleteFile());
    REQUIRE (overdub::song::originalFileFor (location, backingTrack).deleteFile());
    auto reopened = overdub::song::SongPackage::open (location);
    REQUIRE (reopened.has_value());
    REQUIRE (reopened->package->isBackingTrackMissing (backingTrack.id));

    Engine silentEngine { stereoConfig() };
    silentEngine.loadSong (reopened->package->song(), location);
    silentEngine.play();
    DeviceDriver silentDevice { 2, 256 };
    CHECK (nonZeroSamples (silentDevice.render (silentEngine, 4).channels[0]).empty());
}

TEST_CASE ("a loaded plan is pending until the next callback takes it", "[engine][plan]")
{
    TempPackage package;
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = 256 } };
    CHECK (! engine.hasPendingPlan());

    engine.loadSong (overdub::song::makeEmptySong (testSampleRate), package.root);
    CHECK (engine.hasPendingPlan());
    DeviceDriver driver { 2, 256 };
    driver.processOneBlock (engine);
    CHECK (! engine.hasPendingPlan());

    engine.loadSong (overdub::song::makeEmptySong (testSampleRate), package.root);
    CHECK (engine.hasPendingPlan());
    driver.processOneBlock (engine);
    CHECK (! engine.hasPendingPlan());
}
