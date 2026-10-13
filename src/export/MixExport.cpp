#include "export/MixExport.h"

#include "core/Log.h"
#include "export/MixFileWriter.h"
#include "export/MovieExport.h"

#include <algorithm>

namespace overdub::exporting
{
namespace
{
// Where the file is written until it is complete: a hidden sibling, so no reader sees it half-made.
juce::File partialFileFor (const juce::File& destination)
{
    return destination.getSiblingFile ("." + destination.getFileName() + ".partial");
}
} // namespace

core::Expected<void> createExportFolder (const juce::File& destination)
{
    const auto folder = destination.getParentDirectory();
    if (const auto made = folder.createDirectory(); made.failed())
        return std::unexpected (core::Error {
            "export.write_failed",
            "could not create the export folder",
            { { "path", folder.getFullPathName().toStdString() }, { "step", "folder" }, { "detail", made.getErrorMessage().toStdString() } },
        });
    return {};
}

core::Expected<void> moveExportIntoPlace (const juce::File& partial, const juce::File& destination)
{
    if (partial.moveFileTo (destination))
        return {};
    partial.deleteFile();
    return std::unexpected (core::Error {
        "export.write_failed",
        "could not move the exported file into place",
        { { "path", destination.getFullPathName().toStdString() }, { "step", "rename" }, { "from", partial.getFullPathName().toStdString() } },
    });
}

core::Expected<void> writeMixFile (MixRender& render, ExportFormat format, const juce::File& file, const ProgressFn& progress, const ShouldStopFn& shouldStop, song::SampleCount padToSamples)
{
    const auto total = std::max (render.totalSamples(), padToSamples);
    auto writer = MixFileWriter::create (file, format, render.sampleRate(), MixRender::numChannels);
    if (! writer.has_value())
    {
        file.deleteFile(); // the file may exist when a step after creating it failed
        return std::unexpected (writer.error());
    }

    juce::AudioBuffer<float> block { MixRender::numChannels, render.maxBlockSize() };
    song::SampleCount written = 0;
    while (true)
    {
        if (shouldStop && shouldStop())
        {
            (*writer).reset();
            file.deleteFile();
            return std::unexpected (core::Error { "export.cancelled", "the export was stopped", { { "samples_written", written } } });
        }
        auto count = render.renderNext (block.getArrayOfWritePointers(), block.getNumSamples());
        if (count == 0 && written < padToSamples)
        {
            // The mix has ended: silence up to padToSamples.
            count = (int) std::min<song::SampleCount> (block.getNumSamples(), padToSamples - written);
            block.clear();
        }
        if (count == 0)
            break;
        if (auto result = (*writer)->write (block.getArrayOfReadPointers(), count); ! result.has_value())
        {
            (*writer).reset();
            file.deleteFile();
            return std::unexpected (result.error());
        }
        written += count;
        if (progress && total > 0)
            progress ((double) written / (double) total);
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
    // A tail alone is not a mix: without Clips there is no sound to ring out (issue #95).
    const auto total = render.totalSamples();
    if (render.songEnd() <= 0)
        return std::unexpected (core::Error { "export.nothing_to_export", "the Song has no Clips to export", {} });

    if (auto folder = createExportFolder (request.destination); ! folder.has_value())
        return std::unexpected (folder.error());

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
    if (auto moved = moveExportIntoPlace (partial, request.destination); ! moved.has_value())
        return std::unexpected (moved.error());

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

ExportJob::ExportJob (std::unique_ptr<MixRender> mixRender, ExportRequest request, MovieSource movie)
    : juce::Thread ("Overdub export"), render (std::move (mixRender)), exportRequest (std::move (request)), movieSource (std::make_unique<MovieSource> (std::move (movie)))
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
    auto finished = movieSource != nullptr ? exportMovie (*render, exportRequest, *movieSource, progress, shouldStop)
                                           : exportMix (*render, exportRequest, progress, shouldStop);
    result = std::move (finished);
    done.store (true);
}
} // namespace overdub::exporting
