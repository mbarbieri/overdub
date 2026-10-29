#include "song/TransposeRender.h"

#include "song/AtomicWrite.h"
#include "song/AudioFileDecode.h"

#include "AudioMeasurements.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <stop_token>

using Catch::Matchers::WithinAbs;
using namespace overdub::song::testing;

// The Practice Speed render (issue #212, ADR 0009): a source played slower or faster at its own
// pitch, with a Backing Track's Transpose applied in the same pass.
namespace
{
constexpr int sampleRate = 48000;

// A temp folder, deleted with everything in it.
struct ScratchFolder
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-practice-" + juce::Uuid().toString());

    ScratchFolder() { REQUIRE (folder.createDirectory().wasOk()); }
    ~ScratchFolder() { folder.deleteRecursively(); }

    juce::File write (const char* name, const juce::AudioBuffer<float>& audio) const
    {
        const auto file = folder.getChildFile (name);
        REQUIRE (overdub::song::writeFloatWavAtomically (file, audio, sampleRate, {}).has_value());
        return file;
    }
};

juce::AudioBuffer<float> sineBuffer (int channels, int length, double frequency)
{
    juce::AudioBuffer<float> sine { channels, length };
    for (int i = 0; i < length; ++i)
        for (int channel = 0; channel < channels; ++channel)
            sine.setSample (channel, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * i / sampleRate));
    return sine;
}

// The sample with the largest magnitude.
int peakIndex (const float* samples, int length)
{
    int peak = 0;
    for (int i = 1; i < length; ++i)
        if (std::abs (samples[i]) > std::abs (samples[peak]))
            peak = i;
    return peak;
}
} // namespace

TEST_CASE ("a sine rendered at a Practice Speed keeps its pitch and lasts its length divided by the speed", "[song][practice_speed][render]")
{
    const auto speedPercent = GENERATE (80, 50, 25, 125);
    INFO (speedPercent << "%");
    const ScratchFolder scratch;
    const auto source = scratch.write ("take.wav", sineBuffer (2, 2 * sampleRate, 440.0));
    const auto destination = scratch.folder.getChildFile ("take.speed.wav");

    REQUIRE (overdub::song::renderAtPracticeSpeed (source, destination, sampleRate, 0, speedPercent, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    CHECK (rendered->getNumChannels() == 2);
    CHECK (rendered->getNumSamples() == std::llround (2.0 * sampleRate * 100.0 / speedPercent)); // 120000 at 80%
    // Away from both ends, where the stretcher has the whole spectral window. ADR 0005: within 5 cents.
    const auto length = rendered->getNumSamples();
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto measured = measuredFrequency (rendered->getReadPointer (channel), length / 8, length * 7 / 8, sampleRate);
        CHECK_THAT (1200.0 * std::log2 (measured / 440.0), WithinAbs (0.0, 5.0));
    }
}

TEST_CASE ("a render at a Practice Speed also applies a Transpose, in one pass", "[song][practice_speed][render]")
{
    const ScratchFolder scratch;
    const auto source = scratch.write ("backing.wav", sineBuffer (1, 2 * sampleRate, 440.0));
    const auto destination = scratch.folder.getChildFile ("backing.speed.wav");

    REQUIRE (overdub::song::renderAtPracticeSpeed (source, destination, sampleRate, -2, 80, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    CHECK (rendered->getNumSamples() == 120000);
    const auto measured = measuredFrequency (rendered->getReadPointer (0), 15000, 105000, sampleRate);
    CHECK_THAT (1200.0 * std::log2 (measured / (440.0 * std::pow (2.0, -2.0 / 12.0))), WithinAbs (0.0, 5.0)); // 392.00 Hz
}

TEST_CASE ("an impulse rendered at a Practice Speed lands at its time divided by the speed", "[song][practice_speed][render]")
{
    const auto speedPercent = GENERATE (80, 125, 25);
    INFO (speedPercent << "%");
    juce::AudioBuffer<float> click { 1, 2 * sampleRate };
    click.clear();
    click.setSample (0, 48000, 1.0f);
    const ScratchFolder scratch;
    const auto source = scratch.write ("take.wav", click);
    const auto destination = scratch.folder.getChildFile ("take.speed.wav");

    REQUIRE (overdub::song::renderAtPracticeSpeed (source, destination, sampleRate, 0, speedPercent, {}).has_value());

    const auto rendered = overdub::song::readWavFile (destination);
    REQUIRE (rendered.has_value());
    const auto expected = 48000.0 * 100.0 / speedPercent; // 60000 at 80%, 38400 at 125%, 192000 (4t) at 25%
    // ADR 0005: within 1 ms (48 samples) of its time divided by the speed.
    CHECK_THAT ((double) peakIndex (rendered->getReadPointer (0), rendered->getNumSamples()), WithinAbs (expected, 48.0));
}

TEST_CASE ("a Practice Speed render that cannot read its source or is asked to stop writes nothing", "[song][practice_speed][render]")
{
    const ScratchFolder scratch;
    const auto source = scratch.write ("take.wav", sineBuffer (1, sampleRate, 440.0));
    const auto destination = scratch.folder.getChildFile ("take.speed.wav");

    const auto failed = overdub::song::renderAtPracticeSpeed (scratch.folder.getChildFile ("missing.wav"), destination, sampleRate, 0, 80, {});
    REQUIRE_FALSE (failed.has_value());
    CHECK (failed.error().code == "song.practice_render_failed");
    CHECK (failed.error().fields.at ("step") == "read");
    CHECK (failed.error().fields.at ("speed_percent") == 80);

    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = overdub::song::renderAtPracticeSpeed (source, destination, sampleRate, 0, 80, stop.get_token());
    REQUIRE_FALSE (cancelled.has_value());
    CHECK (cancelled.error().code == "song.practice_render_cancelled");

    CHECK_FALSE (destination.exists());
    CHECK_FALSE (overdub::song::tempFileFor (destination).exists());
}

TEST_CASE ("an impulse inside a Loop's range lands at the same Song time in the range render and the whole render", "[song][practice_speed][render][loop]")
{
    const auto speedPercent = GENERATE (80, 125, 25);
    INFO (speedPercent << "%");
    juce::AudioBuffer<float> clicks { 1, 4 * sampleRate };
    clicks.clear();
    clicks.setSample (0, 24000, 1.0f);  // outside the range
    clicks.setSample (0, 100000, 1.0f); // inside it
    const ScratchFolder scratch;
    const auto source = scratch.write ("take.wav", clicks);
    const auto whole = scratch.folder.getChildFile ("take.speed.wav");
    const auto ranged = scratch.folder.getChildFile ("take.speed.range.wav");
    const overdub::song::SampleRange range { 60000, 150000 };

    REQUIRE (overdub::song::renderAtPracticeSpeed (source, whole, sampleRate, 0, speedPercent, {}).has_value());
    REQUIRE (overdub::song::renderAtPracticeSpeed (source, ranged, sampleRate, 0, speedPercent, {}, range).has_value());

    const auto wholeAudio = overdub::song::readWavFile (whole);
    const auto rangedAudio = overdub::song::readWavFile (ranged);
    REQUIRE (wholeAudio.has_value());
    REQUIRE (rangedAudio.has_value());
    REQUIRE (rangedAudio->getNumSamples() == wholeAudio->getNumSamples());
    const auto rate = speedPercent / 100.0;
    const auto rangeStart = (int) std::llround (range.start / rate);
    const auto rangeEnd = (int) std::llround (range.end / rate);
    // ADR 0005: within 1 ms (48 samples) of each other and of its time divided by the speed.
    const auto wholePeak = peakIndex (wholeAudio->getReadPointer (0) + rangeStart, rangeEnd - rangeStart) + rangeStart;
    const auto rangedPeak = peakIndex (rangedAudio->getReadPointer (0), rangedAudio->getNumSamples());
    CHECK_THAT ((double) rangedPeak, WithinAbs ((double) wholePeak, 48.0));
    CHECK_THAT ((double) rangedPeak, WithinAbs (100000.0 / rate, 48.0));
    // Outside the range the render is silent: the impulse at 0.5 s is not in it.
    CHECK_FALSE (rangedAudio->getMagnitude (0, 0, rangeStart) > 0.0f);
    CHECK_FALSE (rangedAudio->getMagnitude (0, rangeEnd, rangedAudio->getNumSamples() - rangeEnd) > 0.0f);
}
