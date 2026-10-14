#include "song/SeparationHelper.h"

#include "separation/OnnxTestModel.h"
#include "song/AudioFileDecode.h"
#include "song/Separation.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

// song::separationHelperProcess, the real side of the helper seam (issue #114): the overdub-separate
// binary started as a child process on the identity stand-in model (OnnxTestModel.h, never the
// real one), its stdout lines read without blocking and its exit reported.
using namespace overdub::song;

namespace
{
struct ProcessFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("overdub-helper-process-test-" + juce::Uuid().toString());
    juce::File model = folder.getChildFile ("model.onnx");
    juce::File input = folder.getChildFile ("input.wav");
    juce::File output = folder.getChildFile ("parts");

    ProcessFixture()
    {
        folder.createDirectory();
        juce::AudioBuffer<float> audio (2, 44100 / 2);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                audio.setSample (c, i, 0.25f * (float) std::sin (2.0 * std::numbers::pi * 440.0 * i / 44100.0));
        REQUIRE (writeFloatWavAtomically (input, audio, 44100, {}).has_value());
    }
    ~ProcessFixture() { folder.deleteRecursively(); }
};

struct Finished
{
    std::vector<nlohmann::json> lines;
    HelperExit exit;
};

// Polls until the helper has ended, as the window's timer would.
Finished pollToEnd (SeparationHelperRun& run)
{
    Finished finished;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto said = run.poll();
        for (const auto& text : said.lines)
        {
            auto line = nlohmann::json::parse (text, nullptr, false);
            INFO ("line: " << text);
            REQUIRE (line.is_object());
            finished.lines.push_back (std::move (line));
        }
        if (said.exit.has_value())
        {
            finished.exit = *said.exit;
            CHECK_FALSE (run.poll().exit.has_value()); // reported once
            return finished;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
    }
    FAIL ("the helper did not end");
    return finished;
}
} // namespace

TEST_CASE ("the helper process reports its progress lines and a successful exit once every part is written", "[song][separation][helper]")
{
    ProcessFixture f;
    REQUIRE (overdub::separation::testing::writeIdentitySeparationModel (f.model, overdub::separation::testing::separationModelMetadata()));

    auto run = separationHelperProcess (juce::File (OVERDUB_SEPARATE_HELPER)) (f.model, f.input, f.output);
    REQUIRE (run.has_value());
    const auto finished = pollToEnd (**run);

    CHECK (finished.exit.succeeded());
    CHECK (finished.exit.errorLine.empty());
    REQUIRE (finished.lines.size() >= 3);
    CHECK (finished.lines.front().at ("event") == "separation.started");
    CHECK (finished.lines.back().at ("event") == "separation.finished");
    const auto chunksDone = std::ranges::count_if (finished.lines, [] (const nlohmann::json& line) { return line.at ("event") == "separation.chunk_done"; });
    CHECK (chunksDone == finished.lines.front().at ("chunks").get<int>());
    for (const auto& part : separationParts)
        CHECK (f.output.getChildFile (juce::String (std::string (part.fileStem)) + ".wav").existsAsFile());
}

TEST_CASE ("the helper process reports a failure's exit code and its stderr line", "[song][separation][helper]")
{
    ProcessFixture f; // no model written

    auto run = separationHelperProcess (juce::File (OVERDUB_SEPARATE_HELPER)) (f.model, f.input, f.output);
    REQUIRE (run.has_value());
    const auto finished = pollToEnd (**run);

    CHECK_FALSE (finished.exit.succeeded());
    CHECK (finished.exit.exitCode == 1);
    CHECK (finished.exit.signal == 0);
    const auto error = nlohmann::json::parse (finished.exit.errorLine, nullptr, false);
    REQUIRE (error.is_object());
    CHECK (error.at ("error") == "separation.model_missing");
}

TEST_CASE ("a killed helper process ends by its signal", "[song][separation][helper]")
{
    ProcessFixture f;
    REQUIRE (overdub::separation::testing::writeIdentitySeparationModel (f.model, overdub::separation::testing::separationModelMetadata()));
    auto run = separationHelperProcess (juce::File (OVERDUB_SEPARATE_HELPER)) (f.model, f.input, f.output);
    REQUIRE (run.has_value());

    (*run)->kill();
    const auto finished = pollToEnd (**run);

    // It may have finished before the kill landed; when it did not, the signal ended it.
    if (! finished.exit.succeeded())
    {
        CHECK (finished.exit.signal == SIGKILL);
        CHECK (finished.exit.exitCode == -1);
    }
}

TEST_CASE ("a helper that cannot be started is an Error naming the executable", "[song][separation][helper]")
{
    ProcessFixture f;
    const auto missing = f.folder.getChildFile ("no-such-helper");

    const auto run = separationHelperProcess (missing) (f.model, f.input, f.output);

    REQUIRE_FALSE (run.has_value());
    CHECK (run.error().code == "song.separation_start_failed");
    CHECK (run.error().fields.at ("path") == missing.getFullPathName().toStdString());
}
