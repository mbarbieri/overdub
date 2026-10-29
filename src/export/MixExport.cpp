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

core::Expected<void> checkExportFolder (const juce::File& destination)
{
    const auto folder = destination.getParentDirectory();
    if (folder.isDirectory())
        return {};
    return std::unexpected (core::Error {
        "export.folder_missing",
        "the export's folder does not exist",
        { { "path", folder.getFullPathName().toStdString() } },
    });
}

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

core::Expected<void> writeMixFile (MixRender& render, ExportFormat format, const juce::File& file, const ProgressFn& progress, const ShouldStopFn& shouldStop, SampleRange range)
{
    auto writer = MixFileWriter::create (file, format, render.sampleRate(), MixRender::numChannels);
    if (! writer.has_value())
    {
        file.deleteFile(); // the file may exist when a step after creating it failed
        return std::unexpected (writer.error());
    }

    juce::AudioBuffer<float> block { MixRender::numChannels, render.maxBlockSize() };
    song::SampleCount position = 0; // the Song sample the next block starts at
    while (position < range.end)
    {
        if (shouldStop && shouldStop())
        {
            (*writer).reset();
            file.deleteFile();
            const auto written = std::max<song::SampleCount> (0, position - range.start);
            return std::unexpected (core::Error { "export.cancelled", "the export was stopped", { { "samples_written", written } } });
        }
        const auto wanted = (int) std::min<song::SampleCount> (block.getNumSamples(), range.end - position);
        auto count = render.renderNext (block.getArrayOfWritePointers(), wanted);
        if (count == 0)
        {
            // The mix has ended: silence up to range.end.
            count = wanted;
            block.clear();
        }
        const auto skipped = (int) std::clamp<song::SampleCount> (range.start - position, 0, count);
        if (skipped < count)
        {
            const float* channels[MixRender::numChannels];
            for (int channel = 0; channel < MixRender::numChannels; ++channel)
                channels[channel] = block.getReadPointer (channel, skipped);
            if (auto result = (*writer)->write (channels, count - skipped); ! result.has_value())
            {
                (*writer).reset();
                file.deleteFile();
                return std::unexpected (result.error());
            }
        }
        position += count;
        if (progress)
            progress ((double) position / (double) range.end);
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
    if (render.songEnd() <= 0)
        return std::unexpected (core::Error { "export.nothing_to_export", "the Song has no Clips to export", {} });
    const auto range = request.loop.has_value() ? request.loop->range : SampleRange { 0, render.totalSamples() };
    const auto total = range.length();

    if (auto folder = checkExportFolder (request.destination); ! folder.has_value())
        return std::unexpected (folder.error());

    core::Fields started { { "format", formatName (request.format) },
                           { "path", request.destination.getFullPathName().toStdString() },
                           { "samples", total },
                           { "song_end_samples", render.songEnd() },
                           { "tail_samples", render.tailSamples() },
                           { "latency_samples", render.latencySamples() },
                           { "sample_rate", render.sampleRate() } };
    if (request.loop.has_value())
    {
        started["loop_id"] = request.loop->loopId;
        started["start_sample"] = range.start;
    }
    core::logEvent (core::Level::info, "export.started", std::move (started));
    const auto startNs = core::hostTimeNowNs();

    const auto partial = partialFileFor (request.destination);
    if (auto written = writeMixFile (render, request.format, partial, progress, shouldStop, range); ! written.has_value())
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
    stop();
}

void ExportJob::stop()
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
