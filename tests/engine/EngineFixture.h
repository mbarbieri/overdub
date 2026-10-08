#pragma once

#include "engine/Engine.h"
#include "song/Song.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Shared helpers for engine tests (ADR 0005): fixture Songs built in code with generated float
// WAV media in a temp package folder, and a loop that drives Engine::process like a device would.
namespace overdub::engine::testing
{
inline constexpr int testSampleRate = 48000;

// A Track at the default mix (0 dB, centre pan) is heard at sqrt (1/2) on each channel: the
// -3 dB constant-power pan law (issue #31, engine/MixLaw.h).
inline const float centrePanGain = (float) std::sqrt (0.5);

// A fresh, empty package folder under the system temp folder, deleted at the end of the test.
struct TempPackage
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("overdub-engine-test", ".overdub", false);

    TempPackage() { root.createDirectory(); }
    ~TempPackage() { root.deleteRecursively(); }
};

// Writes channels (all the same length) as a 32-bit float WAV at the given rate.
inline void writeFloatWav (const juce::File& file, const std::vector<std::vector<float>>& channels, int sampleRate = testSampleRate)
{
    REQUIRE (! channels.empty());
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate (sampleRate)
                             .withNumChannels ((int) channels.size())
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (stream, options);
    REQUIRE (writer != nullptr);

    std::vector<const float*> pointers;
    for (const auto& channel : channels)
        pointers.push_back (channel.data());
    REQUIRE (writer->writeFromFloatArrays (pointers.data(), (int) pointers.size(), (int) channels[0].size()));
}

// A mono signal of `length` zeros with 1.0 at `impulseAt`.
inline std::vector<float> impulse (std::int64_t length, std::int64_t impulseAt, float value = 1.0f)
{
    std::vector<float> samples ((std::size_t) length, 0.0f);
    samples[(std::size_t) impulseAt] = value;
    return samples;
}

// A mono sine of `length` samples at `frequency` Hz.
inline std::vector<float> sine (std::int64_t length, double frequency, float amplitude = 0.5f)
{
    std::vector<float> samples ((std::size_t) length);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = amplitude * (float) std::sin (2.0 * juce::MathConstants<double>::pi * frequency * (double) i / testSampleRate);
    return samples;
}

// Adds a Take with the given channels (1 or 2, same length) to the Song and writes its file.
inline song::Take addMultichannelTake (song::Song& song, const TempPackage& package, const std::string& id, const std::vector<std::vector<float>>& channels)
{
    song::Take take;
    take.id = id;
    take.file = "audio/" + id + ".wav";
    take.channels = (int) channels.size();
    take.lengthSamples = (song::SampleCount) channels[0].size();
    writeFloatWav (package.root.getChildFile (take.file), channels, song.sampleRate);
    song.takes.push_back (take);
    return take;
}

// Adds a mono Take with the given samples to the Song and writes its file into the package.
inline song::Take addTake (song::Song& song, const TempPackage& package, const std::string& id, const std::vector<float>& samples)
{
    song::Take take;
    take.id = id;
    take.file = "audio/" + id + ".wav";
    take.channels = 1;
    take.lengthSamples = (song::SampleCount) samples.size();
    writeFloatWav (package.root.getChildFile (take.file), { samples }, song.sampleRate);
    song.takes.push_back (take);
    return take;
}

inline song::Clip clipOf (const std::string& id, const song::Take& take, song::SampleCount timelineStart, song::SampleCount sourceOffset, song::SampleCount length)
{
    song::Clip clip;
    clip.id = id;
    clip.source = { song::ClipSourceKind::take, take.id };
    clip.timelineStart = timelineStart;
    clip.sourceOffset = sourceOffset;
    clip.length = length;
    return clip;
}

inline song::Track trackWith (const std::string& id, std::vector<song::Clip> clips)
{
    song::Track track;
    track.id = id;
    track.name = id;
    track.clips = std::move (clips);
    return track;
}

// What the device saw: one vector per output channel, every block appended.
struct RenderedOutput
{
    std::vector<std::vector<float>> channels;

    std::int64_t length() const { return channels.empty() ? 0 : (std::int64_t) channels[0].size(); }
};

// Drives Engine::process like a device callback: numBlocks blocks of blockSize samples, with
// fake host times advancing by exactly one block. Buffers are sized before any block runs.
struct DeviceDriver
{
    int numOutputChannels = 2;
    int blockSize = 256;
    std::uint64_t hostTimeNs = 1'000'000'000;
    std::vector<std::vector<float>> outputStorage;
    std::vector<float*> outputPointers;

    DeviceDriver (int outputChannels, int samplesPerBlock)
        : numOutputChannels (outputChannels), blockSize (samplesPerBlock)
    {
        outputStorage.assign ((std::size_t) numOutputChannels, std::vector<float> ((std::size_t) blockSize, 0.0f));
        for (auto& channel : outputStorage)
            outputPointers.push_back (channel.data());
    }

    std::uint64_t blockDurationNs() const { return (std::uint64_t) blockSize * 1'000'000'000ULL / testSampleRate; }

    // One callback; no allocation here, so it may run inside REQUIRE_REALTIME_SAFE.
    void processOneBlock (Engine& engine)
    {
        engine.process (nullptr, 0, outputPointers.data(), numOutputChannels, blockSize, hostTimeNs);
        hostTimeNs += blockDurationNs();
    }

    RenderedOutput render (Engine& engine, int numBlocks)
    {
        RenderedOutput rendered;
        rendered.channels.assign ((std::size_t) numOutputChannels, {});
        for (auto& channel : rendered.channels)
            channel.reserve ((std::size_t) (numBlocks * blockSize));

        for (int block = 0; block < numBlocks; ++block)
        {
            processOneBlock (engine);
            for (std::size_t c = 0; c < outputStorage.size(); ++c)
                rendered.channels[c].insert (rendered.channels[c].end(), outputStorage[c].begin(), outputStorage[c].end());
        }
        return rendered;
    }
};

// A duplex device for recording tests: each block's input is the device's own output stream
// delayed by `loopDelay` samples (output channel c echoes into input channel c, as a cable or a
// speaker and microphone would), plus whatever `extraInput` holds at that stream position.
// loopDelay must be at least one block: a real round trip never reaches into the block being
// rendered. Every input and output sample is kept, indexed by stream position from 0.
struct LoopbackDevice
{
    int numChannels = 2;
    int blockSize = 256;
    std::int64_t loopDelay = 0;
    std::uint64_t hostTimeNs = 1'000'000'000;
    std::vector<std::vector<float>> extraInput; // per channel, by stream position; may be empty
    std::vector<std::vector<float>> outputHistory;
    std::vector<std::vector<float>> inputHistory;
    std::vector<std::vector<float>> outputStorage;
    std::vector<std::vector<float>> inputStorage;
    std::vector<float*> outputPointers;
    std::vector<const float*> inputPointers;

    LoopbackDevice (int channels, int samplesPerBlock, std::int64_t delay)
        : numChannels (channels), blockSize (samplesPerBlock), loopDelay (delay)
    {
        REQUIRE (loopDelay >= blockSize);
        outputStorage.assign ((std::size_t) numChannels, std::vector<float> ((std::size_t) blockSize, 0.0f));
        inputStorage.assign ((std::size_t) numChannels, std::vector<float> ((std::size_t) blockSize, 0.0f));
        outputHistory.assign ((std::size_t) numChannels, {});
        inputHistory.assign ((std::size_t) numChannels, {});
        extraInput.assign ((std::size_t) numChannels, {});
        for (auto& channel : outputStorage)
            outputPointers.push_back (channel.data());
        for (auto& channel : inputStorage)
            inputPointers.push_back (channel.data());
    }

    std::int64_t streamPosition() const { return (std::int64_t) outputHistory[0].size(); }

    std::uint64_t blockDurationNs() const { return (std::uint64_t) blockSize * 1'000'000'000ULL / testSampleRate; }

    // Fills this block's input from the history; may allocate (the histories grow), so tests call
    // it outside REQUIRE_REALTIME_SAFE and then processPreparedBlock inside.
    void prepareInput()
    {
        const auto start = streamPosition();
        for (std::size_t c = 0; c < inputStorage.size(); ++c)
        {
            for (int i = 0; i < blockSize; ++i)
            {
                const auto position = start + i;
                const auto echoed = position - loopDelay;
                float value = echoed >= 0 ? outputHistory[c][(std::size_t) echoed] : 0.0f;
                if (position < (std::int64_t) extraInput[c].size())
                    value += extraInput[c][(std::size_t) position];
                inputStorage[c][(std::size_t) i] = value;
            }
        }
    }

    // One callback with the prepared input; no allocation.
    void processPreparedBlock (Engine& engine)
    {
        engine.process (inputPointers.data(), numChannels, outputPointers.data(), numChannels, blockSize, hostTimeNs);
        hostTimeNs += blockDurationNs();
    }

    void keepHistory()
    {
        for (std::size_t c = 0; c < outputStorage.size(); ++c)
        {
            outputHistory[c].insert (outputHistory[c].end(), outputStorage[c].begin(), outputStorage[c].end());
            inputHistory[c].insert (inputHistory[c].end(), inputStorage[c].begin(), inputStorage[c].end());
        }
    }

    void processOneBlock (Engine& engine)
    {
        prepareInput();
        processPreparedBlock (engine);
        keepHistory();
    }

    void run (Engine& engine, int numBlocks)
    {
        for (int block = 0; block < numBlocks; ++block)
            processOneBlock (engine);
    }
};

// Indices of every sample whose magnitude exceeds `threshold`.
inline std::vector<std::int64_t> nonZeroSamples (const std::vector<float>& samples, float threshold = 0.0f)
{
    std::vector<std::int64_t> found;
    for (std::size_t i = 0; i < samples.size(); ++i)
        if (std::abs (samples[i]) > threshold)
            found.push_back ((std::int64_t) i);
    return found;
}

// Peak of (a - b) in dBFS; -infinity-like (-400) when they are identical.
inline double peakResidualDbfs (const std::vector<float>& a, const std::vector<float>& b)
{
    REQUIRE (a.size() == b.size());
    double peak = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        peak = std::max (peak, std::abs ((double) a[i] - (double) b[i]));
    return peak > 0.0 ? 20.0 * std::log10 (peak) : -400.0;
}
} // namespace overdub::engine::testing
