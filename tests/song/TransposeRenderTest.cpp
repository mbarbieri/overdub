#include "song/TransposeRender.h"

#include "core/Log.h"
#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"
#include "song/BackingTrackImport.h"
#include "song/Transpose.h"

#include "AudioMeasurements.h"
#include "FixtureSongBuilder.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <stop_token>
#include <thread>

using Catch::Matchers::WithinAbs;
using namespace overdub::song::testing;

namespace
{
constexpr int sampleRate = 48000;
constexpr double sineFrequency = 440.0;

// A temp folder holding a 2 s stereo 440 Hz sine as a derived Backing Track WAV.
struct RenderFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transpose-" + juce::Uuid().toString());
    juce::File source = folder.getChildFile ("backing.wav");
    juce::AudioBuffer<float> sine { 2, 2 * sampleRate };

    RenderFixture()
    {
        REQUIRE (folder.createDirectory().wasOk());
        for (int i = 0; i < sine.getNumSamples(); ++i)
            for (int channel = 0; channel < 2; ++channel)
                sine.setSample (channel, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * sineFrequency * i / sampleRate));
        REQUIRE (overdub::song::writeFloatWavAtomically (source, sine, sampleRate, {}).has_value());
    }
    ~RenderFixture() { folder.deleteRecursively(); }
};

// A temp folder, deleted with everything in it.
struct ScratchFolder
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-transpose-" + juce::Uuid().toString());

    ScratchFolder() { REQUIRE (folder.createDirectory().wasOk()); }
    ~ScratchFolder() { folder.deleteRecursively(); }
};
} // namespace

TEST_CASE ("a sine rendered at a Transpose comes out at the computed frequency and the same length", "[song][transpose][render]")
{
    const auto semitones = GENERATE (12, -1);
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");

    REQUIRE (overdub::song::renderTransposed (fixture.source, destination, sampleRate, semitones, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    CHECK (rendered->getNumChannels() == 2);
    CHECK (rendered->getNumSamples() == fixture.sine.getNumSamples());
    const auto expected = sineFrequency * std::pow (2.0, semitones / 12.0); // 880 Hz, 415.30 Hz
    // Measured away from both ends, where the stretcher has the whole spectral window. ADR 0005:
    // within 5 cents.
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto measured = measuredFrequency (rendered->getReadPointer (channel), sampleRate / 4, sampleRate * 7 / 4, sampleRate);
        CHECK_THAT (1200.0 * std::log2 (measured / expected), WithinAbs (0.0, 5.0));
    }
}

TEST_CASE ("a sine is rendered within 5 cents of its shifted pitch across the bass and mid range, at 44.1 and 48 kHz", "[song][transpose][render]")
{
    struct Case
    {
        int rate;
        double frequency;
        int semitones;
    };
    // The cases presetDefault's window put furthest off (up to 17 cents).
    const auto c = GENERATE (Case { 48000, 110.0, 3 }, Case { 48000, 110.0, -2 }, Case { 44100, 220.0, -2 }, Case { 48000, 330.0, -1 });
    INFO (c.frequency << " Hz at " << c.rate << " Hz, " << c.semitones << " semitones");
    const ScratchFolder scratch;
    const auto& folder = scratch.folder;
    juce::AudioBuffer<float> sine { 1, 2 * c.rate };
    for (int i = 0; i < sine.getNumSamples(); ++i)
        sine.setSample (0, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * c.frequency * i / c.rate));
    const auto source = folder.getChildFile ("backing.wav");
    const auto destination = folder.getChildFile ("backing.transpose.wav");
    REQUIRE (overdub::song::writeFloatWavAtomically (source, sine, c.rate, {}).has_value());

    REQUIRE (overdub::song::renderTransposed (source, destination, c.rate, c.semitones, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    const auto expected = c.frequency * std::pow (2.0, c.semitones / 12.0);
    const auto measured = measuredFrequency (rendered->getReadPointer (0), c.rate / 4, c.rate * 7 / 4, c.rate);
    CHECK_THAT (1200.0 * std::log2 (measured / expected), WithinAbs (0.0, 5.0));
}

TEST_CASE ("a render that cannot read its source writes nothing and names the source", "[song][transpose][render]")
{
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");

    const auto failed = overdub::song::renderTransposed (fixture.folder.getChildFile ("missing.wav"), destination, sampleRate, 3, {});

    REQUIRE_FALSE (failed.has_value());
    CHECK (failed.error().code == "song.transpose_render_failed");
    CHECK (failed.error().fields.at ("step") == "read");
    CHECK (failed.error().fields.at ("path") == fixture.folder.getChildFile ("missing.wav").getFullPathName().toStdString());
    CHECK_FALSE (destination.exists());
}

TEST_CASE ("a render asked to stop writes nothing", "[song][transpose][render]")
{
    RenderFixture fixture;
    const auto destination = fixture.folder.getChildFile ("backing.transpose.wav");
    std::stop_source stop;
    stop.request_stop();

    const auto cancelled = overdub::song::renderTransposed (fixture.source, destination, sampleRate, 3, stop.get_token());

    REQUIRE_FALSE (cancelled.has_value());
    CHECK (cancelled.error().code == "song.transpose_render_cancelled");
    CHECK_FALSE (destination.exists());
    CHECK_FALSE (overdub::song::tempFileFor (destination).exists());
}

namespace
{
// Runs a TransposeRun to the end, as the window's timer would.
std::optional<overdub::core::Expected<void>> runToEnd (overdub::song::TransposeRun& run)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto ended = run.advance (overdub::song::TransposeRun::Clock::now()))
            return ended;
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
    FAIL ("the Transpose run did not end");
    return std::nullopt;
}

std::vector<nlohmann::json> eventsNamed (const overdub::core::ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            found.push_back (event);
    return found;
}
} // namespace

TEST_CASE ("a Song at Transpose 0 needs no render, and at +2 one per Backing Track without one", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band").backingTrack ("Click").build();
    const auto root = fixture->package->location();
    auto song = fixture->song();

    CHECK (overdub::song::missingTransposeRenders (root, song).empty());

    song.transpose = 2;
    const auto missing = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (missing.size() == 2);
    CHECK (missing[0].backingTrackId == song.backingTracks[0].id);
    CHECK (missing[0].source == overdub::song::derivedFileFor (root, song.backingTracks[0]));
    CHECK (missing[0].destination == overdub::song::transposedFileFor (root, song.backingTracks[0], 2));

    REQUIRE (missing[0].destination.replaceWithText ("rendered"));
    const auto stillMissing = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (stillMissing.size() == 1);
    CHECK (stillMissing[0].backingTrackId == song.backingTracks[1].id);
}

TEST_CASE ("only the renders of the kept Transpose stay in the package", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band").build();
    const auto root = fixture->package->location();
    const auto& backingTrack = fixture->song().backingTracks[0];
    for (const int semitones : { -1, 2, 12 })
        REQUIRE (overdub::song::transposedFileFor (root, backingTrack, semitones).replaceWithText ("rendered"));

    CHECK (overdub::song::deleteTransposeRendersExcept (root, 2) == 2);

    CHECK (overdub::song::transposedFileFor (root, backingTrack, 2).existsAsFile());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, -1).exists());
    CHECK_FALSE (overdub::song::transposedFileFor (root, backingTrack, 12).exists());
    CHECK (overdub::song::derivedFileFor (root, backingTrack).existsAsFile());
    CHECK (overdub::song::originalFileFor (root, backingTrack).existsAsFile());
}

TEST_CASE ("a Transpose run renders every missing render on a worker and logs its start and end", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).backingTrack ("Click", 1.0).build();
    const auto root = fixture->package->location();
    auto song = fixture->song();
    song.transpose = -1;
    overdub::core::ScopedLogCapture capture;

    auto run = overdub::song::TransposeRun::start (song.sampleRate, song.transpose, overdub::song::missingTransposeRenders (root, song), overdub::song::TransposeRun::Clock::now());
    CHECK (run->semitones() == -1);
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    CHECK (ended->has_value());
    CHECK (overdub::song::missingTransposeRenders (root, song).empty());
    const auto started = eventsNamed (capture, "song.transpose_started");
    REQUIRE (started.size() == 1);
    CHECK (started[0].at ("semitones") == -1);
    CHECK (started[0].at ("renders") == 2);
    const auto rendered = eventsNamed (capture, "song.transpose_rendered");
    REQUIRE (rendered.size() == 1);
    CHECK (rendered[0].at ("renders") == 2);
    CHECK (rendered[0].contains ("duration_ms"));
}

TEST_CASE ("a Transpose run that cannot render names the Backing Track", "[song][transpose][render]")
{
    auto fixture = FixtureSongBuilder().backingTrack ("Band", 1.0).build();
    const auto root = fixture->package->location();
    auto song = fixture->song();
    song.transpose = 4;
    auto jobs = overdub::song::missingTransposeRenders (root, song);
    REQUIRE (overdub::song::derivedFileFor (root, song.backingTracks[0]).deleteFile());

    auto run = overdub::song::TransposeRun::start (song.sampleRate, song.transpose, jobs, overdub::song::TransposeRun::Clock::now());
    const auto ended = runToEnd (*run);

    REQUIRE (ended.has_value());
    REQUIRE_FALSE (ended->has_value());
    CHECK (ended->error().code == "song.transpose_render_failed");
    CHECK (ended->error().fields.at ("backing_track_id") == song.backingTracks[0].id);
    CHECK_FALSE (jobs[0].destination.exists());
}
