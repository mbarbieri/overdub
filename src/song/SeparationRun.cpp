#include "song/SeparationRun.h"

#include "core/Log.h"
#include "song/BackingTrackImport.h"
#include "song/Separation.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace overdub::song
{
namespace
{
// The helper's stderr line is {"error": <code>, "message": ...}; anything else is kept as text.
core::Error helperFailed (const HelperExit& exit)
{
    core::Fields fields { { "exit_code", exit.exitCode }, { "signal", exit.signal }, { "helper_error", "" }, { "helper_message", exit.errorLine } };
    const auto parsed = nlohmann::json::parse (exit.errorLine, nullptr, false);
    if (parsed.is_object())
    {
        if (const auto code = parsed.find ("error"); code != parsed.end() && code->is_string())
            fields["helper_error"] = *code;
        if (const auto message = parsed.find ("message"); message != parsed.end() && message->is_string())
            fields["helper_message"] = *message;
    }
    return core::Error { "song.separation_failed", "the Separation helper failed", std::move (fields) };
}

int intField (const nlohmann::json& line, const char* key)
{
    const auto found = line.find (key);
    return found != line.end() && found->is_number_integer() ? found->get<int>() : 0;
}
} // namespace

core::Expected<std::unique_ptr<SeparationRun>> SeparationRun::start (SongPackage& package, const Id& trackId, const SeparationTools& tools, Clock::time_point now)
{
    const auto sourceId = separableBackingTrack (package.song(), trackId);
    if (! sourceId.has_value())
        return std::unexpected (sourceId.error());
    if (! tools.startHelper)
        return std::unexpected (core::Error { "song.separation_unavailable", "this build has no Separation helper", {} });
    if (! tools.model.existsAsFile())
        return std::unexpected (core::Error { "song.separation_model_missing", "the Separation model is not installed", { { "path", pathOf (tools.model) } } });

    const auto& backingTracks = package.song().backingTracks;
    const auto source = *std::ranges::find (backingTracks, *sourceId, &BackingTrack::id);
    const auto input = originalFileFor (package.location(), source);
    const auto output = tools.workFolder.getChildFile ("overdub-separation-" + juce::Uuid().toString());
    const auto model = separationModelName (tools.model);

    auto helper = tools.startHelper (tools.model, input, output);
    if (! helper.has_value())
        return std::unexpected (helper.error());

    core::logEvent (core::Level::info,
                    "song.separation_started",
                    { { "track_id", trackId }, { "source_backing_track_id", source.id }, { "model", model }, { "input", pathOf (input) }, { "output", pathOf (output) } });
    auto run = std::unique_ptr<SeparationRun> (new SeparationRun (package, trackId, source.id, source.originalFilename, model, output, now));
    run->helper = std::move (*helper);
    return run;
}

SeparationRun::SeparationRun (SongPackage& songPackage, Id track, Id sourceBackingTrack, std::string sourceName, std::string modelName, juce::File outputFolder, Clock::time_point startTime)
    : package (songPackage),
      trackId (std::move (track)),
      sourceBackingTrackId (std::move (sourceBackingTrack)),
      sourceFileName (std::move (sourceName)),
      model (std::move (modelName)),
      output (std::move (outputFolder)),
      startedAt (startTime)
{
}

SeparationRun::~SeparationRun()
{
    if (helper != nullptr)
        helper->kill();
    helper.reset();
    output.deleteRecursively();
}

std::optional<core::Expected<std::vector<Id>>> SeparationRun::advance (Clock::time_point now)
{
    if (helper == nullptr)
        return std::nullopt;
    auto said = helper->poll();
    readProgress (said.lines);
    if (! said.exit.has_value())
        return std::nullopt;

    helper.reset();
    auto result = finish (*said.exit, now);
    output.deleteRecursively();
    return result;
}

void SeparationRun::readProgress (const std::vector<std::string>& lines)
{
    for (const auto& text : lines)
    {
        const auto line = nlohmann::json::parse (text, nullptr, false);
        if (! line.is_object() || ! line.contains ("event") || ! line.at ("event").is_string())
            continue;
        const auto event = line.at ("event").get<std::string>();
        if (event == "separation.started")
            shownProgress.chunks = intField (line, "chunks");
        if (event == "separation.chunk_done")
            shownProgress = SeparationProgress { intField (line, "chunk"), intField (line, "chunks") };
    }
}

core::Expected<std::vector<Id>> SeparationRun::finish (const HelperExit& exit, Clock::time_point now)
{
    if (! exit.succeeded())
        return std::unexpected (helperFailed (exit));

    std::vector<juce::File> partFiles;
    for (const auto& part : separationParts)
    {
        const auto file = output.getChildFile (juce::String (std::string (part.fileStem)) + ".wav");
        if (! file.existsAsFile())
        {
            auto error = helperFailed (exit);
            error.message = "the Separation helper did not write every part";
            error.fields["reason"] = "part_missing";
            error.fields["path"] = pathOf (file);
            return std::unexpected (std::move (error));
        }
        partFiles.push_back (file);
    }

    auto added = package.addSeparation (trackId, partFiles, model, now);
    if (! added.has_value())
        return added;

    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
    core::logEvent (core::Level::info,
                    "song.separation_finished",
                    {
                        { "track_id", trackId },
                        { "source_backing_track_id", sourceBackingTrackId },
                        { "backing_track_ids", *added },
                        { "model", model },
                        { "duration_ms", durationMs },
                    });
    return added;
}
} // namespace overdub::song
