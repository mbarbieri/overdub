#pragma once

#include "separation/OnnxTestModel.h"
#include "song/AudioFileDecode.h"
#include "song/Separation.h"
#include "song/SeparationHelper.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

// A fake overdub-separate (issue #114, ADR 0007: tests never run the real model) behind the
// song::StartSeparationHelper seam. Each poll() is one step of the helper's protocol
// (src/separation/SeparateCommand.h): separation.started, one separation.chunk_done per chunk,
// then it writes six known sines (one frequency per part, 32-bit float stereo at 44.1 kHz) into
// the output folder with their separation.part_written lines and separation.finished, and exits.
// A script can make it fail instead (an exit code, a signal, or no start at all), write fewer
// parts, or mix lines that are not JSON events into its progress.
namespace overdub::song::testing
{
inline constexpr int helperSampleRate = 44100;
inline constexpr float partAmplitude = 0.25f;

// The frequency of each part's sine, in separationParts order.
inline constexpr std::array<double, 6> partFrequencies { 110.0, 220.0, 330.0, 440.0, 550.0, 660.0 };

inline float partSample (std::size_t part, std::int64_t sample)
{
    return partAmplitude * (float) std::sin (2.0 * std::numbers::pi * partFrequencies[part] * (double) sample / helperSampleRate);
}

struct FakeHelperScript
{
    int chunks = 3;
    int partLengthSamples = 2 * helperSampleRate;
    std::size_t partsWritten = 6; // the first n parts in separationParts order
    int exitCode = 0;
    int signal = 0;              // non-zero: a signal ends it instead (exit code -1)
    std::string errorLine;       // stderr on a failure
    bool malformedLines = false; // a non-JSON line and a JSON line without an event before each chunk
    std::optional<core::Error> startError; // the helper never starts
};

// What the fake saw, shared with the test.
struct FakeHelperLog
{
    struct Start
    {
        juce::File model;
        juce::File input;
        juce::File outputFolder;
    };
    std::vector<Start> starts;
    int polls = 0;
    bool killed = false;
};

class FakeSeparationHelperRun final : public SeparationHelperRun
{
public:
    FakeSeparationHelperRun (FakeHelperScript helperScript, juce::File outputFolder, std::shared_ptr<FakeHelperLog> helperLog)
        : script (std::move (helperScript)),
          output (std::move (outputFolder)),
          log (std::move (helperLog))
    {
    }

    HelperOutput poll() override
    {
        ++log->polls;
        HelperOutput said;
        if (ended)
            return said;
        if (step == 0)
            said.lines.push_back (R"({"event":"separation.started","chunks":)" + std::to_string (script.chunks) + R"(,"samples":)" + std::to_string (script.partLengthSamples) + "}");
        else if (step <= script.chunks)
        {
            if (script.malformedLines)
            {
                said.lines.push_back ("onnxruntime warning: not json");
                said.lines.push_back (R"({"chunk":1})");
                said.lines.push_back (R"({"event":"separation.chunk_done","chunk":)");
            }
            said.lines.push_back (R"({"event":"separation.chunk_done","chunk":)" + std::to_string (step) + R"(,"chunks":)" + std::to_string (script.chunks) + "}");
        }
        else
            finish (said);
        ++step;
        return said;
    }

    void kill() override
    {
        if (! ended)
            log->killed = true;
        ended = true;
    }

private:
    void finish (HelperOutput& said)
    {
        const auto succeeds = script.exitCode == 0 && script.signal == 0;
        if (succeeds)
            writeParts (said);
        if (succeeds)
            said.lines.push_back (R"({"event":"separation.finished","seconds":1.5,"peak_memory_mb":12})");
        if (script.signal != 0)
            said.exit = HelperExit { .exitCode = -1, .signal = script.signal, .errorLine = script.errorLine };
        else
            said.exit = HelperExit { .exitCode = script.exitCode, .errorLine = script.errorLine };
        ended = true;
    }

    void writeParts (HelperOutput& said) const
    {
        REQUIRE (output.createDirectory().wasOk());
        for (std::size_t part = 0; part < std::min (script.partsWritten, separationParts.size()); ++part)
        {
            juce::AudioBuffer<float> audio (2, script.partLengthSamples);
            for (int i = 0; i < script.partLengthSamples; ++i)
                for (int channel = 0; channel < 2; ++channel)
                    audio.setSample (channel, i, partSample (part, i));
            const auto file = output.getChildFile (juce::String (std::string (separationParts[part].fileStem)) + ".wav");
            REQUIRE (writeFloatWavAtomically (file, audio, helperSampleRate, {}).has_value());
            said.lines.push_back (R"({"event":"separation.part_written","part":")" + std::string (separationParts[part].fileStem) + "\"}");
        }
    }

    FakeHelperScript script;
    juce::File output;
    std::shared_ptr<FakeHelperLog> log;
    int step = 0;
    bool ended = false;
};

inline StartSeparationHelper fakeSeparationHelper (FakeHelperScript script, std::shared_ptr<FakeHelperLog> log)
{
    return [script, log] (const juce::File& model, const juce::File& input, const juce::File& outputFolder) -> core::Expected<std::unique_ptr<SeparationHelperRun>> {
        log->starts.push_back ({ model, input, outputFolder });
        if (script.startError.has_value())
            return std::unexpected (*script.startError);
        return std::make_unique<FakeSeparationHelperRun> (script, outputFolder, log);
    };
}

// A file standing in for the installed model: the stand-in ONNX graph with the real model's
// metadata, so the app's model check passes; the fake helper never reads it.
inline juce::File writeStandInModel (const juce::File& folder, const std::map<std::string, std::string>& metadata = overdub::separation::testing::separationModelMetadata())
{
    const auto model = folder.getChildFile ("models").getChildFile ("bs_roformer_sw.fp32.onnx");
    REQUIRE (model.getParentDirectory().createDirectory().wasOk());
    REQUIRE (overdub::separation::testing::writeIdentitySeparationModel (model, metadata));
    return model;
}
} // namespace overdub::song::testing
