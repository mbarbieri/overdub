#include "separation/SeparateCommand.h"

#include "OnnxTestModel.h"
#include "song/AudioFileDecode.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

extern char** environ;

using namespace overdub::separation::testing;

// The overdub-separate binary run as a child process, as Overdub will run it (issue #113), with
// the identity stand-in model (OnnxTestModel.h) in place of BS-RoFormer SW.
namespace
{
const std::vector<std::string> partNames { "bass", "drums", "other", "vocals", "guitar", "piano" };

struct HelperRun
{
    int exitCode = -1;
    std::vector<nlohmann::json> stdoutLines;
    std::vector<nlohmann::json> stderrLines;
};

std::vector<nlohmann::json> jsonLines (const juce::File& file)
{
    std::vector<nlohmann::json> lines;
    juce::StringArray texts;
    texts.addLines (file.loadFileAsString());
    texts.removeEmptyStrings();
    for (const auto& text : texts)
    {
        auto parsed = nlohmann::json::parse (text.toStdString(), nullptr, false);
        INFO ("line: " << text);
        REQUIRE_FALSE (parsed.is_discarded());
        REQUIRE (parsed.is_object());
        lines.push_back (std::move (parsed));
    }
    return lines;
}

struct HelperFixture
{
    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("overdub-separate-test-" + juce::Uuid().toString());
    juce::File model = folder.getChildFile ("model.onnx");
    juce::File output = folder.getChildFile ("parts");

    HelperFixture() { folder.createDirectory(); }
    ~HelperFixture()
    {
        output.setReadOnly (false, true);
        folder.deleteRecursively();
    }

    HelperRun run (const std::vector<std::string>& arguments) const
    {
        const auto outFile = folder.getChildFile ("stdout.txt");
        const auto errFile = folder.getChildFile ("stderr.txt");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init (&actions);
        posix_spawn_file_actions_addopen (&actions, 1, outFile.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_addopen (&actions, 2, errFile.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        std::vector<std::string> argv { OVERDUB_SEPARATE_HELPER };
        argv.insert (argv.end(), arguments.begin(), arguments.end());
        std::vector<char*> pointers;
        for (auto& argument : argv)
            pointers.push_back (argument.data());
        pointers.push_back (nullptr);

        pid_t pid = 0;
        const auto spawned = posix_spawn (&pid, OVERDUB_SEPARATE_HELPER, &actions, nullptr, pointers.data(), environ);
        posix_spawn_file_actions_destroy (&actions);
        REQUIRE (spawned == 0);
        int status = 0;
        REQUIRE (waitpid (pid, &status, 0) == pid);
        REQUIRE (WIFEXITED (status));
        return HelperRun { WEXITSTATUS (status), jsonLines (outFile), jsonLines (errFile) };
    }

    HelperRun separate (const juce::File& input) const
    {
        return run ({ model.getFullPathName().toStdString(), input.getFullPathName().toStdString(), output.getFullPathName().toStdString() });
    }
};

juce::File writeSineWav (const juce::File& file, int sampleRate, int channels, int length)
{
    juce::AudioBuffer<float> audio (channels, length);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < length; ++i)
            audio.setSample (c, i, (0.5f / (float) (c + 1)) * (float) std::sin (2.0 * std::numbers::pi * 440.0 * (c + 1) * i / sampleRate));
    REQUIRE (overdub::song::writeFloatWavAtomically (file, audio, sampleRate, {}).has_value());
    return file;
}

void requireOneError (const HelperRun& run, const std::string& code)
{
    CHECK (run.exitCode == 1);
    REQUIRE (run.stderrLines.size() == 1);
    CHECK (run.stderrLines[0]["error"] == code);
}

struct PartFile
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 0;
    unsigned int bitsPerSample = 0;
    bool isFloat = false;
};

PartFile readPart (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader { wav.createReaderFor (new juce::FileInputStream (file), true) };
    REQUIRE (reader != nullptr);
    PartFile part { juce::AudioBuffer<float> ((int) reader->numChannels, (int) reader->lengthInSamples), reader->sampleRate, reader->bitsPerSample, reader->usesFloatingPointData };
    REQUIRE (reader->read (&part.audio, 0, part.audio.getNumSamples(), 0, true, true));
    return part;
}
} // namespace

TEST_CASE ("overdub-separate writes six 32-bit float stereo parts as long as the input, with progress", "[separation][helper]")
{
    HelperFixture fixture;
    REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
    const int length = 44100 + 123;
    const auto input = writeSineWav (fixture.folder.getChildFile ("mix.wav"), 44100, 2, length);

    const auto run = fixture.separate (input);

    CHECK (run.exitCode == 0);
    CHECK (run.stderrLines.empty());
    REQUIRE (run.stdoutLines.size() == 1 + 1 + 6 + 1);
    CHECK (run.stdoutLines[0]["event"] == "separation.started");
    CHECK (run.stdoutLines[0]["chunks"] == 1);
    CHECK (run.stdoutLines[0]["samples"] == length);
    CHECK (run.stdoutLines[1] == nlohmann::json { { "event", "separation.chunk_done" }, { "chunk", 1 }, { "chunks", 1 } });
    for (std::size_t p = 0; p < partNames.size(); ++p)
    {
        CHECK (run.stdoutLines[2 + p]["event"] == "separation.part_written");
        CHECK (run.stdoutLines[2 + p]["part"] == partNames[p]);
    }
    CHECK (run.stdoutLines[8]["event"] == "separation.finished");

    const auto original = overdub::song::readWavFile (input);
    REQUIRE (original.has_value());
    for (const auto& name : partNames)
    {
        INFO ("part " << name);
        const auto part = readPart (fixture.output.getChildFile (name + ".wav"));
        CHECK (part.sampleRate == 44100.0);
        CHECK (part.bitsPerSample == 32);
        CHECK (part.isFloat);
        REQUIRE (part.audio.getNumChannels() == 2);
        REQUIRE (part.audio.getNumSamples() == length);
        float worst = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < length; ++i)
                worst = std::max (worst, std::abs (part.audio.getSample (c, i) - original->getSample (c, i)));
        CHECK (worst < 1e-5f); // the identity model: every part is the input
    }
    CHECK (fixture.output.findChildFiles (juce::File::findFiles, false, "*.tmp").isEmpty());
}

TEST_CASE ("overdub-separate decodes a mono 48 kHz input to 44.1 kHz stereo parts as long as the decoded input", "[separation][helper]")
{
    HelperFixture fixture;
    REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
    const auto input = writeSineWav (fixture.folder.getChildFile ("mono48k.wav"), 48000, 1, 48000);
    const auto decoded = overdub::song::decodeAudioFile (input, 44100);
    REQUIRE (decoded.has_value());

    const auto run = fixture.separate (input);

    CHECK (run.exitCode == 0);
    for (const auto& name : partNames)
    {
        const auto part = readPart (fixture.output.getChildFile (name + ".wav"));
        CHECK (part.sampleRate == 44100.0);
        REQUIRE (part.audio.getNumChannels() == 2);
        CHECK (part.audio.getNumSamples() == decoded->audio.getNumSamples());
    }
}

TEST_CASE ("overdub-separate fails with one machine-readable error line", "[separation][helper]")
{
    HelperFixture fixture;
    const auto input = writeSineWav (fixture.folder.getChildFile ("mix.wav"), 44100, 2, 4410);

    SECTION ("when the model is missing")
    {
        const auto run = fixture.separate (input);
        requireOneError (run, "separation.model_missing");
        CHECK (run.stderrLines[0]["path"] == fixture.model.getFullPathName().toStdString());
        CHECK (run.stdoutLines.empty());
    }
    SECTION ("when the model's metadata does not describe the expected graph")
    {
        auto metadata = separationModelMetadata();
        metadata["architecture"] = "htdemucs";
        REQUIRE (writeIdentitySeparationModel (fixture.model, metadata));
        const auto run = fixture.separate (input);
        requireOneError (run, "separation.model_mismatch");
        CHECK (run.stderrLines[0]["key"] == "architecture");
    }
    SECTION ("when the model's segment is not the expected one")
    {
        auto metadata = separationModelMetadata();
        metadata["segment_samples"] = "409600";
        REQUIRE (writeIdentitySeparationModel (fixture.model, metadata));
        const auto run = fixture.separate (input);
        requireOneError (run, "separation.model_mismatch");
        CHECK (run.stderrLines[0]["key"] == "segment_samples");
    }
    SECTION ("when the model is not an ONNX graph")
    {
        REQUIRE (fixture.model.replaceWithText ("not a model"));
        const auto run = fixture.separate (input);
        requireOneError (run, "separation.model_unloadable");
    }
    SECTION ("when the input is unreadable")
    {
        REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
        const auto notAudio = fixture.folder.getChildFile ("notes.txt");
        REQUIRE (notAudio.replaceWithText ("not audio"));
        const auto run = fixture.separate (notAudio);
        requireOneError (run, "separation.input_unreadable");
        CHECK (run.stderrLines[0]["path"] == notAudio.getFullPathName().toStdString());
    }
    SECTION ("when the input does not exist")
    {
        REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
        const auto run = fixture.separate (fixture.folder.getChildFile ("missing.wav"));
        requireOneError (run, "separation.input_unreadable");
    }
    SECTION ("when the output folder is not writable")
    {
        REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
        REQUIRE (fixture.output.createDirectory().wasOk());
        REQUIRE (chmod (fixture.output.getFullPathName().toRawUTF8(), 0555) == 0);
        const auto run = fixture.separate (input);
        requireOneError (run, "separation.output_unwritable");
        CHECK (run.stderrLines[0]["path"] == fixture.output.getFullPathName().toStdString());
    }
    SECTION ("when the output folder cannot be created")
    {
        REQUIRE (writeIdentitySeparationModel (fixture.model, separationModelMetadata()));
        const auto blocker = fixture.folder.getChildFile ("file");
        REQUIRE (blocker.replaceWithText ("x"));
        const auto run = fixture.run ({ fixture.model.getFullPathName().toStdString(), input.getFullPathName().toStdString(), blocker.getChildFile ("parts").getFullPathName().toStdString() });
        requireOneError (run, "separation.output_unwritable");
    }
}

TEST_CASE ("overdub-separate exits 2 on wrong usage", "[separation][helper]")
{
    HelperFixture fixture;
    const auto run = fixture.run ({ "only-one-argument" });
    CHECK (run.exitCode == overdub::separation::exitUsage);
    REQUIRE (run.stderrLines.size() == 1);
    CHECK (run.stderrLines[0]["error"] == "separation.usage");
}
