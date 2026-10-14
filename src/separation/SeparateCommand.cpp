#include "separation/SeparateCommand.h"

#include "core/Error.h"
#include "separation/CrossFade.h"
#include "separation/GraphSession.h"
#include "separation/ModelCheck.h"
#include "separation/SeparationPipeline.h"
#include "song/AudioFileDecode.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <exception>

namespace overdub::separation
{
namespace
{
void writeLine (std::ostream& stream, const nlohmann::json& line)
{
    stream << line.dump() << std::endl; // endl flushes: the parent reads progress as it happens
}

int fail (std::ostream& err, const core::Error& error)
{
    nlohmann::json line { { "error", error.code }, { "message", error.message } };
    if (error.fields.is_object())
        for (const auto& [key, value] : error.fields.items())
            line[key] = value;
    writeLine (err, line);
    return error.code == "separation.usage" ? exitUsage : exitFailure;
}

std::string pathOf (const juce::File& file)
{
    return file.getFullPathName().toStdString();
}

juce::File resolve (const std::string& argument)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argument));
}

long long peakMemoryMegabytes()
{
    rusage usage {};
    getrusage (RUSAGE_SELF, &usage);
    return (long long) usage.ru_maxrss / (1024 * 1024); // ru_maxrss is in bytes on macOS
}

core::Expected<void> checkModelExists (const juce::File& model)
{
    if (model.existsAsFile())
        return {};
    return std::unexpected (core::Error { "separation.model_missing", "the Separation model is not installed", { { "path", pathOf (model) } } });
}

// Creates the folder if needed and proves a file can be created in it before any long work.
core::Expected<void> checkOutputWritable (const juce::File& folder)
{
    const auto unwritable = [&] (const char* step) {
        return std::unexpected (core::Error { "separation.output_unwritable", "the output folder is not writable", { { "path", pathOf (folder) }, { "step", step } } });
    };
    if (! folder.isDirectory() && ! folder.createDirectory().wasOk())
        return unwritable ("create_folder");
    // POSIX rather than juce::File, whose debug assertions print to stderr on failure.
    const auto probe = folder.getChildFile (".overdub-separate-probe").getFullPathName().toStdString();
    const int descriptor = ::open (probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0)
        return unwritable ("create_file");
    ::close (descriptor);
    ::unlink (probe.c_str());
    return {};
}

// The input at the model's rate, as stereo.
core::Expected<juce::AudioBuffer<float>> decodeInput (const juce::File& input)
{
    auto decoded = song::decodeAudioFile (input, modelSampleRate);
    if (! decoded.has_value())
    {
        auto fields = decoded.error().fields.is_object() ? decoded.error().fields : nlohmann::json::object();
        fields["path"] = pathOf (input);
        fields["cause"] = decoded.error().code;
        return std::unexpected (core::Error { "separation.input_unreadable", "the input audio file could not be decoded", fields });
    }
    if (decoded->audio.getNumSamples() == 0)
        return std::unexpected (core::Error { "separation.input_unreadable", "the input audio file is empty", { { "path", pathOf (input) }, { "cause", "empty" } } });

    auto& audio = decoded->audio;
    if (audio.getNumChannels() == 2)
        return std::move (audio);
    juce::AudioBuffer<float> stereo (2, audio.getNumSamples());
    stereo.copyFrom (0, 0, audio, 0, 0, audio.getNumSamples());
    stereo.copyFrom (1, 0, audio, 0, 0, audio.getNumSamples());
    return stereo;
}

core::Expected<void> writeParts (const std::vector<juce::AudioBuffer<float>>& parts, const juce::File& folder, std::ostream& out)
{
    for (std::size_t p = 0; p < parts.size(); ++p)
    {
        const auto destination = folder.getChildFile (juce::String (expectedParts[p]) + ".wav");
        if (auto written = song::writeFloatWavAtomically (destination, parts[p], modelSampleRate, {}); ! written.has_value())
        {
            auto fields = written.error().fields.is_object() ? written.error().fields : nlohmann::json::object();
            fields["path"] = pathOf (destination);
            fields["cause"] = written.error().code;
            return std::unexpected (core::Error { "separation.part_write_failed", "a part file could not be written", fields });
        }
        writeLine (out, { { "event", "separation.part_written" }, { "part", expectedParts[p] }, { "path", pathOf (destination) } });
    }
    return {};
}

int runChecked (const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err)
{
    const auto started = std::chrono::steady_clock::now();
    if (arguments.size() != 3)
        return fail (err, core::Error { "separation.usage", "usage: overdub-separate <model.onnx> <input audio file> <output folder>", { { "arguments", arguments.size() } } });

    const auto modelFile = resolve (arguments[0]);
    const auto inputFile = resolve (arguments[1]);
    const auto outputFolder = resolve (arguments[2]);

    if (auto exists = checkModelExists (modelFile); ! exists.has_value())
        return fail (err, exists.error());
    if (auto writable = checkOutputWritable (outputFolder); ! writable.has_value())
        return fail (err, writable.error());
    auto input = decodeInput (inputFile);
    if (! input.has_value())
        return fail (err, input.error());

    auto session = GraphSession::open (pathOf (modelFile));
    if (! session.has_value())
        return fail (err, session.error());
    const auto facts = (*session)->facts();
    if (! facts.has_value())
        return fail (err, facts.error());
    const auto shape = checkModelFacts (*facts, pathOf (modelFile));
    if (! shape.has_value())
        return fail (err, shape.error());

    const int samples = input->getNumSamples();
    writeLine (out, { { "event", "separation.started" }, { "chunks", chunkCountFor (samples, shape->segment) }, { "samples", samples }, { "parts", expectedParts } });
    const auto reportChunk = [&] (int chunk, int chunks) {
        writeLine (out, { { "event", "separation.chunk_done" }, { "chunk", chunk }, { "chunks", chunks } });
    };
    const auto parts = separateAudio (*input, *shape, (*session)->runner (*shape), reportChunk);
    if (! parts.has_value())
        return fail (err, parts.error());
    if (auto written = writeParts (*parts, outputFolder, out); ! written.has_value())
        return fail (err, written.error());

    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    writeLine (out, { { "event", "separation.finished" }, { "seconds", seconds }, { "peak_memory_mb", peakMemoryMegabytes() } });
    return exitSuccess;
}
} // namespace

int runSeparateCommand (const std::vector<std::string>& arguments, std::ostream& out, std::ostream& err)
{
    try
    {
        return runChecked (arguments, out, err);
    }
    catch (const std::exception& error) // std::bad_alloc on a machine without the ~6 GB the graph needs
    {
        return fail (err, core::Error { "separation.internal_error", "the helper stopped on an unexpected exception", { { "detail", error.what() } } });
    }
}
} // namespace overdub::separation
