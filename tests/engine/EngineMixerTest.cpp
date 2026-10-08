#include "engine/Engine.h"

#include "EngineFixture.h"
#include "support/RealtimeGuardCatch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <set>
#include <string>
#include <vector>

// The Mixer heard through the engine (issue #31, ADR 0005: sines for the pan law, null tests for
// volume, mute and solo). Expectations are computed here from the spec, not from the engine:
//   volume  gain = 10^(volumeDb / 20)
//   pan     the -3 dB constant-power law: left = cos (theta), right = sin (theta),
//           theta = (pan + 1) * pi / 4; centre = sqrt (1/2) on each side (-3.01 dB)
//   stereo  as balance: the left channel times the left gain, the right times the right gain
//   mute    silences its Track; solo-in-place: when any Track is soloed, every Track that is not
//           soloed is silent (a soloed and muted Track is silent too)
//   Master  10^(masterVolumeDb / 20) on the sum
using overdub::engine::Engine;
using overdub::engine::EngineConfig;
using namespace overdub::engine::testing;
using Catch::Matchers::WithinAbs;
namespace song = overdub::song;

namespace
{
constexpr int blockSize = 256;
constexpr int numBlocks = 40; // 10240 samples
constexpr std::int64_t sourceLength = 10240;
const double centreGain = std::sqrt (0.5);

double dbGain (double db)
{
    return std::pow (10.0, db / 20.0);
}

std::vector<float> scaled (const std::vector<float>& samples, double gain)
{
    std::vector<float> out (samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        out[i] = (float) ((double) samples[i] * gain);
    return out;
}

std::vector<float> summed (const std::vector<std::vector<float>>& signals, std::size_t length)
{
    std::vector<float> out (length, 0.0f);
    for (const auto& signal : signals)
        for (std::size_t i = 0; i < length; ++i)
            out[i] += signal[i];
    return out;
}

std::vector<float> slice (const std::vector<float>& samples, std::int64_t from, std::int64_t to)
{
    return { samples.begin() + from, samples.begin() + to };
}

// One mono Track per sine, each a whole-Song Clip; Track ids "a", "b", "c", ...
struct SineTracks
{
    TempPackage package;
    song::Song song = song::makeEmptySong (testSampleRate);
    std::vector<std::vector<float>> sources;

    explicit SineTracks (const std::vector<double>& frequencies)
    {
        for (std::size_t i = 0; i < frequencies.size(); ++i)
        {
            const auto id = std::string (1, (char) ('a' + i));
            sources.push_back (sine (sourceLength, frequencies[i], 0.3f));
            const auto take = addTake (song, package, "take-" + id, sources.back());
            song.tracks.push_back (trackWith (id, { clipOf ("clip-" + id, take, 0, 0, sourceLength) }));
        }
    }

    song::Track& track (const std::string& id)
    {
        const auto found = std::ranges::find (song.tracks, id, &song::Track::id);
        REQUIRE (found != song.tracks.end());
        return *found;
    }
};

RenderedOutput renderSong (const song::Song& songToRender, const TempPackage& package)
{
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (songToRender, package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    return device.render (engine, numBlocks);
}
} // namespace

TEST_CASE ("a sine panned hard left, centre and hard right gives the constant-power gains per channel", "[engine][mixer]")
{
    SineTracks tracks { { 997.0 } };
    const auto& source = tracks.sources[0];

    struct PanCase
    {
        double pan;
        double left;
        double right;
    };
    const auto panCase = GENERATE_COPY (PanCase { -1.0, 1.0, 0.0 }, PanCase { 0.0, centreGain, centreGain }, PanCase { 1.0, 0.0, 1.0 });
    INFO ("pan " << panCase.pan);
    tracks.track ("a").pan = panCase.pan;

    const auto rendered = renderSong (tracks.song, tracks.package);
    CHECK (peakResidualDbfs (rendered.channels[0], scaled (source, panCase.left)) <= -120.0);
    CHECK (peakResidualDbfs (rendered.channels[1], scaled (source, panCase.right)) <= -120.0);
}

TEST_CASE ("centre pan is -3 dB on each channel", "[engine][mixer]")
{
    SineTracks tracks { { 440.0 } };
    const auto rendered = renderSong (tracks.song, tracks.package);

    const auto peakDb = [] (const std::vector<float>& samples) {
        float peak = 0.0f;
        for (const auto sample : samples)
            peak = std::max (peak, std::abs (sample));
        return 20.0 * std::log10 ((double) peak);
    };
    const auto sourceDb = peakDb (tracks.sources[0]);
    CHECK_THAT (peakDb (rendered.channels[0]) - sourceDb, WithinAbs (-3.0103, 0.001));
    CHECK_THAT (peakDb (rendered.channels[1]) - sourceDb, WithinAbs (-3.0103, 0.001));
}

TEST_CASE ("a stereo source is panned as balance: each channel keeps its own side", "[engine][mixer]")
{
    TempPackage package;
    auto songToPlay = song::makeEmptySong (testSampleRate);
    const auto left = sine (sourceLength, 330.0, 0.4f);
    const auto right = sine (sourceLength, 1250.0, 0.25f);
    const auto take = addMultichannelTake (songToPlay, package, "take-st", { left, right });
    songToPlay.tracks.push_back (trackWith ("st", { clipOf ("clip-st", take, 0, 0, sourceLength) }));

    SECTION ("centre: both sides at -3 dB")
    {
        const auto rendered = renderSong (songToPlay, package);
        CHECK (peakResidualDbfs (rendered.channels[0], scaled (left, centreGain)) <= -120.0);
        CHECK (peakResidualDbfs (rendered.channels[1], scaled (right, centreGain)) <= -120.0);
    }

    SECTION ("hard left: the left side at full level, the right side silent")
    {
        songToPlay.tracks[0].pan = -1.0;
        const auto rendered = renderSong (songToPlay, package);
        CHECK (peakResidualDbfs (rendered.channels[0], left) <= -120.0);
        CHECK (nonZeroSamples (rendered.channels[1]).empty());
    }

    SECTION ("half right: theta = 3 pi / 8")
    {
        songToPlay.tracks[0].pan = 0.5;
        const auto rendered = renderSong (songToPlay, package);
        CHECK (peakResidualDbfs (rendered.channels[0], scaled (left, std::cos (3.0 * juce::MathConstants<double>::pi / 8.0))) <= -120.0);
        CHECK (peakResidualDbfs (rendered.channels[1], scaled (right, std::sin (3.0 * juce::MathConstants<double>::pi / 8.0))) <= -120.0);
    }
}

TEST_CASE ("Track and Master volume null against the computed dB gain", "[engine][mixer]")
{
    SineTracks tracks { { 523.0, 1777.0 } };
    const auto trackDb = GENERATE (-60.0, -6.0, 0.0, 3.5, 12.0);
    const auto masterDb = GENERATE (-12.0, 0.0, 6.0);
    INFO ("Track a at " << trackDb << " dB, Master at " << masterDb << " dB");
    tracks.track ("a").volumeDb = trackDb;
    tracks.song.master.volumeDb = masterDb;

    const auto rendered = renderSong (tracks.song, tracks.package);
    const auto expected = scaled (summed ({ scaled (tracks.sources[0], dbGain (trackDb)), tracks.sources[1] }, (std::size_t) sourceLength), centreGain * dbGain (masterDb));
    CHECK (peakResidualDbfs (rendered.channels[0], expected) <= -120.0);
    CHECK (peakResidualDbfs (rendered.channels[1], expected) <= -120.0);
}

TEST_CASE ("mute and solo combinations null against the sum of the Tracks that should sound", "[engine][mixer]")
{
    SineTracks tracks { { 220.0, 660.0, 1500.0 } };

    struct MixCase
    {
        const char* name;
        std::set<std::string> muted;
        std::set<std::string> soloed;
        std::set<std::string> heard;
    };
    const auto mixCase = GENERATE (MixCase { "nothing muted or soloed", {}, {}, { "a", "b", "c" } },
                                   MixCase { "one muted", { "b" }, {}, { "a", "c" } },
                                   MixCase { "all muted", { "a", "b", "c" }, {}, {} },
                                   MixCase { "one soloed", {}, { "b" }, { "b" } },
                                   MixCase { "two soloed", {}, { "a", "c" }, { "a", "c" } },
                                   MixCase { "soloed and muted, with another soloed", { "a" }, { "a", "c" }, { "c" } },
                                   MixCase { "the only soloed Track muted", { "b" }, { "b" }, {} },
                                   MixCase { "one soloed, another muted", { "c" }, { "a" }, { "a" } });
    INFO (mixCase.name);
    for (auto& track : tracks.song.tracks)
    {
        track.mute = mixCase.muted.contains (track.id);
        track.solo = mixCase.soloed.contains (track.id);
    }

    std::vector<std::vector<float>> heard;
    for (std::size_t i = 0; i < tracks.song.tracks.size(); ++i)
        if (mixCase.heard.contains (tracks.song.tracks[i].id))
            heard.push_back (tracks.sources[i]);
    const auto expected = scaled (summed (heard, (std::size_t) sourceLength), centreGain);

    const auto rendered = renderSong (tracks.song, tracks.package);
    CHECK (peakResidualDbfs (rendered.channels[0], expected) <= -120.0);
    CHECK (peakResidualDbfs (rendered.channels[1], expected) <= -120.0);
}

TEST_CASE ("a mix change during playback is heard from the next block, without reloading the Song", "[engine][mixer]")
{
    SineTracks tracks { { 440.0, 880.0 } };
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (tracks.song, tracks.package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    const auto before = device.render (engine, 4); // Song samples 0 .. 1023
    CHECK (peakResidualDbfs (before.channels[0], scaled (summed (tracks.sources, 1024), centreGain)) <= -120.0);

    auto changed = tracks.song;
    changed.tracks[0].volumeDb = -6.0;
    changed.tracks[1].mute = true;
    changed.master.volumeDb = -3.0;
    REQUIRE (engine.setMix (changed));

    // The first block after the change ramps from the old gains to the new ones; from the second
    // block on the output nulls against the new mix. The plan was not swapped: no stale silence.
    const auto after = device.render (engine, 8); // Song samples 1024 .. 3071
    const auto expected = scaled (slice (tracks.sources[0], 1024, 3072), dbGain (-6.0) * centreGain * dbGain (-3.0));
    CHECK (peakResidualDbfs (slice (after.channels[0], 256, 2048), slice (expected, 256, 2048)) <= -120.0);
    CHECK (peakResidualDbfs (slice (after.channels[1], 256, 2048), slice (expected, 256, 2048)) <= -120.0);
    // The ramp block only moves down from the old gains to the new ones: nothing in it is louder
    // than both sines at their old, centred level.
    float rampPeak = 0.0f;
    for (std::size_t i = 0; i < 256; ++i)
        rampPeak = std::max (rampPeak, std::abs (after.channels[0][i]));
    CHECK (rampPeak <= (float) (2 * 0.3 * centreGain) + 1.0e-6f);
}

TEST_CASE ("setMix refuses a Song whose Tracks are not the loaded ones", "[engine][mixer]")
{
    overdub::core::ScopedLogCapture capture;
    SineTracks tracks { { 440.0, 880.0 } };
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    CHECK (! engine.setMix (tracks.song)); // nothing loaded yet

    engine.loadSong (tracks.song, tracks.package.root);
    auto reordered = tracks.song;
    std::swap (reordered.tracks[0], reordered.tracks[1]);
    CHECK (! engine.setMix (reordered));

    auto fewer = tracks.song;
    fewer.tracks.pop_back();
    CHECK (! engine.setMix (fewer));

    CHECK (engine.setMix (tracks.song));

    std::vector<std::string> reasons;
    std::size_t applied = 0;
    for (const auto& event : capture.events())
    {
        if (event.at ("event") == "engine.mix_refused")
            reasons.push_back (event.at ("reason"));
        applied += event.at ("event") == "engine.mix_applied" ? 1 : 0;
    }
    CHECK (reasons == std::vector<std::string> { "no_plan", "tracks_differ", "tracks_differ" });
    CHECK (applied == 1);
}

TEST_CASE ("the meters report each Track's post-fader peak and the Master's, then start again", "[engine][mixer][meters]")
{
    TempPackage package;
    auto songToPlay = song::makeEmptySong (testSampleRate);
    const auto takeA = addTake (songToPlay, package, "take-a", impulse (4000, 1000, 0.5f));
    const auto takeB = addTake (songToPlay, package, "take-b", impulse (4000, 3000, -0.8f));
    songToPlay.tracks.push_back (trackWith ("a", { clipOf ("clip-a", takeA, 0, 0, 4000) }));
    songToPlay.tracks.push_back (trackWith ("b", { clipOf ("clip-b", takeB, 0, 0, 4000) }));
    songToPlay.tracks[0].volumeDb = -6.0;
    songToPlay.tracks[1].pan = 1.0;
    songToPlay.master.volumeDb = 6.0;

    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (songToPlay, package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };

    device.render (engine, 8); // Song samples 0 .. 2047: only a's impulse
    auto peaks = engine.takeMeterPeaks();
    REQUIRE (peaks.tracks.size() == 2);
    CHECK_THAT (peaks.tracks.at ("a"), WithinAbs (0.5 * dbGain (-6.0) * centreGain, 1.0e-6));
    CHECK_THAT (peaks.tracks.at ("b"), WithinAbs (0.0, 0.0));
    CHECK_THAT (peaks.master, WithinAbs (0.5 * dbGain (-6.0) * centreGain * dbGain (6.0), 1.0e-6));

    device.render (engine, 8); // 2048 .. 4095: only b's impulse, hard right at full level
    peaks = engine.takeMeterPeaks();
    CHECK_THAT (peaks.tracks.at ("a"), WithinAbs (0.0, 0.0));
    CHECK_THAT (peaks.tracks.at ("b"), WithinAbs (0.8, 1.0e-6));
    CHECK_THAT (peaks.master, WithinAbs (0.8 * dbGain (6.0), 1.0e-6));

    engine.stop();
    device.render (engine, 2);
    peaks = engine.takeMeterPeaks();
    CHECK_THAT (peaks.tracks.at ("a"), WithinAbs (0.0, 0.0));
    CHECK_THAT (peaks.master, WithinAbs (0.0, 0.0));
}

TEST_CASE ("process() neither allocates nor locks while mix parameters change during playback", "[engine][mixer][rt]")
{
    SineTracks tracks { { 440.0, 880.0, 1320.0 } };
    Engine engine { EngineConfig { .numOutputChannels = 2, .maxBlockSize = blockSize } };
    engine.loadSong (tracks.song, tracks.package.root);
    engine.play();
    DeviceDriver device { 2, blockSize };
    REQUIRE_REALTIME_SAFE (device.processOneBlock (engine));

    for (int step = 0; step < 12; ++step)
    {
        auto changed = tracks.song;
        changed.tracks[(std::size_t) step % 3].volumeDb = -3.0 * step;
        changed.tracks[(std::size_t) (step + 1) % 3].pan = (step % 5 - 2) / 2.0;
        changed.tracks[(std::size_t) (step + 2) % 3].mute = step % 2 == 0;
        changed.tracks[(std::size_t) step % 3].solo = step % 3 == 0;
        changed.master.volumeDb = -1.0 * step;
        REQUIRE (engine.setMix (changed));
        REQUIRE_REALTIME_SAFE (device.processOneBlock (engine); device.processOneBlock (engine));
        engine.takeMeterPeaks();
    }
}
