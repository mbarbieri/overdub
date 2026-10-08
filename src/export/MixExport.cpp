#include "export/MixExport.h"

#include "core/Log.h"
#include "export/MixFileWriter.h"
#include "export/MovieExport.h"

#include <algorithm>

namespace overdub::exporting
{
namespace
{
core::Error folderFailed (const juce::File& folder, const juce::Result& result)
{
    return core::Error {
        "export.write_failed",
        "could not create the export folder",
        { { "path", folder.getFullPathName().toStdString() }, { "step", "folder" }, { "detail", result.getErrorMessage().toStdString() } },
    };
}

// Where the file is written until it is complete: a hidden sibling, so no reader sees it half-made.
juce::File partialFileFor (const juce::File& destination)
{
    return destination.getSiblingFile ("." + destination.getFileName() + ".partial");
}
} // namespace

core::Expected<void> writeMixFile (MixRender& render, ExportFormat format, const juce::File& file, const ProgressFn& progress, const ShouldStopFn& shouldStop)
{
    const auto total = render.totalSamples();
    auto writer = MixFileWriter::create (file, format, render.sampleRate(), MixRender::numChannels);
    if (! writer.has_value())
    {
        file.deleteFile(); // the file may exist when a step after creating it failed
        return std::unexpected (writer.error());
    }

    juce::AudioBuffer<float> block { MixRender::numChannels, render.maxBlockSize() };
    while (true)
    {
        if (shouldStop && shouldStop())
        {
            (*writer).reset();
            file.deleteFile();
            return std::unexpected (core::Error { "export.cancelled", "the export was stopped", { { "samples_written", render.renderedSamples() } } });
        }
        const auto count = render.renderNext (block.getArrayOfWritePointers(), block.getNumSamples());
        if (count == 0)
            break;
        if (auto written = (*writer)->write (block.getArrayOfReadPointers(), count); ! written.has_value())
        {
            (*writer).reset();
            file.deleteFile();
            return std::unexpected (written.error());
        }
        if (progress && total > 0)
            progress ((double) render.renderedSamples() / (double) total);
    }

    if (auto closed = (*writer)->close(); ! closed.has_value())
    {
        file.deleteFile();
        return std::unexpected (closed.error());
    }
    return {};
}

core::Expected<ExportResult> exportMix (MixRender& render, const ExportRequest& request, const ProgressFn& progress, const ShouldStopFn& shouldStop)
{
    const auto total = render.totalSamples();
    if (total <= 0)
        return std::unexpected (core::Error { "export.nothing_to_export", "the Song has no Clips to export", {} });

    const auto folder = request.destination.getParentDirectory();
    if (const auto made = folder.createDirectory(); made.failed())
        return std::unexpected (folderFailed (folder, made));

    core::logEvent (core::Level::info,
                    "export.started",
                    { { "format", formatName (request.format) },
                      { "path", request.destination.getFullPathName().toStdString() },
                      { "samples", total },
                      { "song_end_samples", render.songEnd() },
                      { "tail_samples", render.tailSamples() },
                      { "latency_samples", render.latencySamples() },
                      { "sample_rate", render.sampleRate() } });
    const auto startNs = core::hostTimeNowNs();

    const auto partial = partialFileFor (request.destination);
    if (auto written = writeMixFile (render, request.format, partial, progress, shouldStop); ! written.has_value())
        return std::unexpected (written.error());
    if (! partial.moveFileTo (request.destination))
    {
        partial.deleteFile();
        return std::unexpected (core::Error {
            "export.write_failed",
            "could not move the exported mix into place",
            { { "path", request.destination.getFullPathName().toStdString() }, { "step", "rename" }, { "from", partial.getFullPathName().toStdString() } },
        });
    }

    const auto elapsedSeconds = (double) (core::hostTimeNowNs() - startNs) / 1.0e9;
    const auto audioSeconds = (double) total / render.sampleRate();
    core::logEvent (core::Level::info,
                    "export.finished",
                    { { "format", formatName (request.format) },
                      { "path", request.destination.getFullPathName().toStdString() },
                      { "samples", total },
                      { "elapsed_ms", elapsedSeconds * 1000.0 },
                      { "speed", elapsedSeconds > 0.0 ? audioSeconds / elapsedSeconds : 0.0 } });
    return ExportResult { request.destination, total };
}

ExportJob::ExportJob (std::unique_ptr<MixRender> mixRender, ExportRequest request)
    : juce::Thread ("Overdub export"), render (std::move (mixRender)), exportRequest (std::move (request))
{
    startThread();
}

ExportJob::ExportJob (std::unique_ptr<MixRender> mixRender, ExportRequest request, MovieRequest movie)
    : juce::Thread ("Overdub export"), render (std::move (mixRender)), exportRequest (std::move (request)), movieRequest (std::make_unique<MovieRequest> (std::move (movie)))
{
    startThread();
}

ExportJob::~ExportJob()
{
    signalThreadShouldExit();
    stopThread (-1);
}

std::optional<core::Expected<ExportResult>> ExportJob::takeResult()
{
    if (! done.load())
        return std::nullopt;
    auto taken = std::move (result);
    result.reset();
    return taken;
}

void ExportJob::run()
{
    const core::ScopedThreadRole role { core::ThreadRole::io };
    const auto progress = [this] (double fraction) { fractionDone.store (std::clamp (fraction, 0.0, 1.0)); };
    const auto shouldStop = [this] { return threadShouldExit(); };
    auto finished = movieRequest != nullptr ? exportMovie (*render, *movieRequest, progress, shouldStop)
                                            : exportMix (*render, exportRequest, progress, shouldStop);
    result = std::move (finished);
    done.store (true);
}
} // namespace overdub::exporting
