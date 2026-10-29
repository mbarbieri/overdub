#include "engine/Engine.h"

#include "EngineFixture.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The band filter heard through the engine (issue #234, ADR 0010; ADR 0005: sines and null
// tests). A Track's strip runs a second-order Butterworth high-pass at the low cut and low-pass at
// the high cut, so (from the spec, not the engine): a sine an octave below the low cut, or an
// octave above the high cut, is attenuated by at least 12 dB; one at the band's centre, four
// octaves wide here, is within 1 dB; with the filter off, or on another Track, the output nulls.
using namespace overdub::engine;
using namespace overdub::engine::testing;
using Catch::Matchers::WithinAbs;
namespace song = overdub::song;

namespace
{
constexpr int blockSize = 256;
constexpr int numBlocks = 150; // 38400 samples
constexpr std::int64_t sourceLength = 400 * blockSize; // longer than any test plays
constexpr int lowCutHz = 200;
constexpr int highCutHz = 3200;
// Measured after the filters have settled: the slowest pole (the 200 Hz high-pass) decays within
// a few ms; the first quarter of a second is left out.
constexpr std::int64_t measureFrom = 12000;

// One mono Track per sine, each a whole-Song Clip; Track ids "a", "b".
struct SineTracks
{
    TempPackage package;
    song::Song song = song::makeEmptySong (testSampleRate);

    explicit SineTracks (const std::vector<double>& frequencies)
    {
        for (std::size_t i = 0; i < frequencies.size(); ++i)
        {
            const auto id = std::string (1, (char) ('a' + i));
            const auto take = addTake (song, package, "take-" + id, sine (sourceLength, frequencies[i], 0.3f));
            song.tracks.push_back (trackWith (id, { clipOf ("clip-" + id, take, 0, 0, sourceLength) }));
        }
    }
};

RenderedOutput render (const SineTracks& tracks, const std::optional<HeardBandFilter>& filter)
{
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.setBandFilter (filter);
    engine.loadSong (tracks.song, tracks.package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    return device.render (engine, numBlocks);
}

double rmsFrom (const std::vector<float>& samples, std::int64_t from)
{
    double sum = 0.0;
    for (auto i = (std::size_t) from; i < samples.size(); ++i)
        sum += (double) samples[i] * (double) samples[i];
    return std::sqrt (sum / (double) (samples.size() - (std::size_t) from));
}

// The filtered output's level against the unfiltered one's, in dB, channel 0.
double filteredGainDb (double frequency)
{
    SineTracks tracks { { frequency } };
    const auto unfiltered = render (tracks, std::nullopt);
    const auto filtered = render (tracks, HeardBandFilter { .trackId = "a", .lowHz = lowCutHz, .highHz = highCutHz });
    return 20.0 * std::log10 (rmsFrom (filtered.channels[0], measureFrom) / rmsFrom (unfiltered.channels[0], measureFrom));
}
} // namespace

TEST_CASE ("a sine an octave below the low cut or an octave above the high cut is attenuated by at least 12 dB", "[engine][band_filter]")
{
    const auto frequency = GENERATE (lowCutHz / 2.0, highCutHz * 2.0);
    INFO ("sine at " << frequency << " Hz");
    CHECK (filteredGainDb (frequency) <= -12.0);
}

TEST_CASE ("a sine at the band's centre is within 1 dB of unfiltered", "[engine][band_filter]")
{
    const auto centre = std::sqrt ((double) lowCutHz * highCutHz); // 800 Hz
    CHECK_THAT (filteredGainDb (centre), WithinAbs (0.0, 1.0));
}

TEST_CASE ("with the filter off, or on another Track, the output nulls against the unfiltered one", "[engine][band_filter]")
{
    SineTracks tracks { { 100.0, 6400.0 } };
    tracks.song.tracks[1].mute = true; // only Track "a" is heard
    const auto unfiltered = render (tracks, std::nullopt);

    const auto onTheOtherTrack = render (tracks, HeardBandFilter { .trackId = "b", .lowHz = lowCutHz, .highHz = highCutHz });
    const auto onNoTrackOfTheSong = render (tracks, HeardBandFilter { .trackId = "gone", .lowHz = lowCutHz, .highHz = highCutHz });

    CHECK (peakResidualDbfs (onTheOtherTrack.channels[0], unfiltered.channels[0]) <= -120.0);
    CHECK (peakResidualDbfs (onTheOtherTrack.channels[1], unfiltered.channels[1]) <= -120.0);
    CHECK (peakResidualDbfs (onNoTrackOfTheSong.channels[0], unfiltered.channels[0]) <= -120.0);
}

TEST_CASE ("the band filter follows setBandFilter from the next block without a new plan, kept across loadSong, and runs realtime-safe", "[engine][band_filter][realtime]")
{
    SineTracks tracks { { 100.0 } };
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (tracks.song, tracks.package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    const auto unfiltered = rmsFrom (device.render (engine, 60).channels[0], 0);

    engine.setBandFilter (HeardBandFilter { .trackId = "a", .lowHz = lowCutHz, .highHz = highCutHz });
    CHECK_FALSE (engine.hasPendingPlan());
    REQUIRE_REALTIME_SAFE (for (int block = 0; block < 60; ++block) device.processOneBlock (engine));
    const auto filtered = rmsFrom (device.render (engine, 60).channels[0], 0);
    CHECK (20.0 * std::log10 (filtered / unfiltered) <= -12.0);

    SECTION ("a plan loaded after keeps the filter")
    {
        engine.loadSong (tracks.song, tracks.package.root);
        device.render (engine, 60); // the new plan's first blocks: stale output, then settling
        CHECK (20.0 * std::log10 (rmsFrom (device.render (engine, 60).channels[0], 0) / unfiltered) <= -12.0);
    }

    SECTION ("moving a cut changes it at once, and none turns it off")
    {
        engine.setBandFilter (HeardBandFilter { .trackId = "a", .lowHz = 50, .highHz = highCutHz });
        REQUIRE_REALTIME_SAFE (for (int block = 0; block < 20; ++block) device.processOneBlock (engine));
        CHECK_THAT (20.0 * std::log10 (rmsFrom (device.render (engine, 60).channels[0], 0) / unfiltered), WithinAbs (0.0, 1.0));

        engine.setBandFilter (std::nullopt);
        REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));
        CHECK_THAT (20.0 * std::log10 (rmsFrom (device.render (engine, 60).channels[0], 0) / unfiltered), WithinAbs (0.0, 0.01));
    }
}
