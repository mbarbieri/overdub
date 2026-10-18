#include "song/SampleRateConversion.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "song/BackingTrackImport.h"
#include "song/PackageFolders.h"
#include "song/Peaks.h"
#include "song/SongPackage.h"

#include <algorithm>
#include <cmath>

namespace overdub::song
{
SampleCount convertedSamples (SampleCount samples, int fromRate, int toRate)
{
    return (SampleCount) std::llround ((double) samples * (double) toRate / (double) fromRate);
}

std::string conversionStepLabel (int toRate)
{
    return "Convert to " + std::to_string (toRate) + " Hz";
}

namespace
{
std::optional<SampleCount> mediaLength (const Song& song, const ClipSource& source)
{
    if (source.kind == ClipSourceKind::take)
    {
        const auto found = std::ranges::find (song.takes, source.id, &Take::id);
        return found != song.takes.end() ? std::optional (found->lengthSamples) : std::nullopt;
    }
    const auto found = std::ranges::find (song.backingTracks, source.id, &BackingTrack::id);
    return found != song.backingTracks.end() ? std::optional (found->lengthSamples) : std::nullopt;
}

Id convertedId (const ClipSource& source, const ConvertedMedia& media)
{
    if (source.kind == ClipSourceKind::take)
    {
        const auto found = media.takes.find (source.id);
        return found != media.takes.end() ? found->second.id : source.id;
    }
    const auto found = media.backingTracks.find (source.id);
    return found != media.backingTracks.end() ? found->second.id : source.id;
}

Clip convertedClip (Clip clip, const Song& converted, int fromRate, int toRate, const ConvertedMedia& media)
{
    const auto convert = [&] (SampleCount samples) { return convertedSamples (samples, fromRate, toRate); };
    const auto start = convert (clip.timelineStart);
    const auto offset = convert (clip.sourceOffset);
    clip.length = convert (clip.timelineStart + clip.length) - start;
    clip.timelineStart = start;
    clip.sourceOffset = offset;
    clip.source.id = convertedId (clip.source, media);
    if (const auto available = mediaLength (converted, clip.source))
    {
        clip.sourceOffset = std::min (clip.sourceOffset, *available);
        clip.length = std::min (clip.length, *available - clip.sourceOffset);
    }
    clip.fadeInLength = std::min (convert (clip.fadeInLength), clip.length);
    clip.fadeOutLength = std::min (convert (clip.fadeOutLength), clip.length - clip.fadeInLength);
    return clip;
}
} // namespace

Song convertedSong (const Song& song, int toRate, const ConvertedMedia& media)
{
    const auto fromRate = song.sampleRate;
    const auto convert = [&] (SampleCount samples) { return convertedSamples (samples, fromRate, toRate); };

    Song converted = song;
    converted.sampleRate = toRate;
    converted.playhead = convert (song.playhead);
    for (auto& pass : converted.passes)
        pass.startSample = convert (pass.startSample);

    for (auto& take : converted.takes)
    {
        const auto origin = convert (take.origin);
        const auto compensation = TakeCompensation { convert (take.compensation.reportedLatencySamples), convert (take.compensation.calibrationOffsetSamples) };
        if (const auto found = media.takes.find (take.id); found != media.takes.end())
            take = found->second;
        else
            take.lengthSamples = convert (take.lengthSamples);
        take.origin = origin;
        take.compensation = compensation;
    }

    for (auto& backingTrack : converted.backingTracks)
    {
        if (const auto found = media.backingTracks.find (backingTrack.id); found != media.backingTracks.end())
            backingTrack = found->second;
        else
            backingTrack.lengthSamples = convert (backingTrack.lengthSamples);
        if (const auto source = media.backingTracks.find (backingTrack.separatedFrom.backingTrackId); source != media.backingTracks.end())
            backingTrack.separatedFrom.backingTrackId = source->second.id;
    }

    for (auto& videoTake : converted.videoTakes)
    {
        videoTake.startSample = convert (videoTake.startSample);
        videoTake.calibrationOffsetSamples = convert (videoTake.calibrationOffsetSamples);
        videoTake.songSamplesPerMovieSecond = videoTake.songSamplesPerMovieSecond * (double) toRate / (double) fromRate;
    }

    for (auto& track : converted.tracks)
        for (auto& clip : track.clips)
            clip = convertedClip (clip, converted, fromRate, toRate, media);
    return converted;
}

namespace
{
core::Error conversionFailed (const juce::File& file, const Id& mediaId, const char* step, const core::Error& cause)
{
    return core::Error { "song.conversion_failed",
                         "a Take or Backing Track could not be converted",
                         { { "path", pathOf (file) }, { "media_id", mediaId }, { "step", step }, { "cause", cause.code } } };
}

void writePeaksOrWarn (const juce::File& packageRoot, const Id& mediaId, const juce::AudioBuffer<float>& audio)
{
    if (auto peaks = writePeaksFile (peaksFileFor (packageRoot, mediaId), audio, {}); ! peaks.has_value())
        core::logError (core::Error { "song.peaks_write_failed", "could not write the peaks cache", peaks.error().fields }, core::Level::warn);
}

// A Take's WAV at `toRate` under a new id, added to `media`; nothing when its file is missing.
core::Expected<void> convertTake (const juce::File& packageRoot, const Take& take, int toRate, ConvertedMedia& media)
{
    const auto source = packageRoot.getChildFile (take.file);
    if (! source.existsAsFile())
        return {};
    auto decoded = decodeAudioFile (source, toRate);
    if (! decoded.has_value())
        return std::unexpected (conversionFailed (source, take.id, "decode", decoded.error()));

    Take converted = take;
    converted.id = makeId();
    converted.file = std::string (audioFolderName) + "/" + converted.id + ".wav";
    converted.lengthSamples = decoded->audio.getNumSamples();
    media.takes[take.id] = converted; // before the write, so a failed one is deleted too
    const auto destination = packageRoot.getChildFile (converted.file);
    if (auto written = writeFloatWavAtomically (destination, decoded->audio, toRate, {}); ! written.has_value())
        return std::unexpected (conversionFailed (destination, take.id, "write", written.error()));
    writePeaksOrWarn (packageRoot, converted.id, decoded->audio);
    return {};
}

// A Backing Track derived again at `toRate` under a new id from its original (its derived WAV when
// the original is gone), or none when both are missing.
core::Expected<std::optional<BackingTrack>> convertBackingTrack (const juce::File& packageRoot, const BackingTrack& backingTrack, int toRate)
{
    const auto original = originalFileFor (packageRoot, backingTrack);
    const auto derived = derivedFileFor (packageRoot, backingTrack);
    const auto source = original.existsAsFile() ? original : derived;
    if (! source.existsAsFile())
        return std::nullopt;
    auto written = writeBackingTrackFiles (packageRoot, source, toRate, {});
    if (! written.has_value())
        return std::unexpected (conversionFailed (source, backingTrack.id, "derive", written.error()));

    BackingTrack converted = backingTrack;
    converted.id = written->id;
    converted.originalExtension = written->originalExtension; // the copy's name on disk
    converted.channels = written->channels;
    converted.lengthSamples = written->lengthSamples;
    if (source == derived)
    {
        converted.originalSampleRate = written->originalSampleRate;
        converted.formatName = written->formatName;
    }
    return converted;
}
} // namespace

core::Expected<ConvertedMedia> convertMediaFiles (const juce::File& packageRoot, const Song& song, int toRate, ConversionProgress& progress, std::stop_token stop)
{
    progress.files = (int) (song.takes.size() + song.backingTracks.size());
    ConvertedMedia media;
    const auto failed = [&] (core::Error error) -> core::Expected<ConvertedMedia> {
        deleteConvertedMedia (packageRoot, media);
        return std::unexpected (std::move (error));
    };
    const auto cancelled = [&] {
        return failed (core::Error { "song.conversion_cancelled", "the conversion was cancelled", {} });
    };

    for (const auto& take : song.takes)
    {
        if (stop.stop_requested())
            return cancelled();
        if (auto converted = convertTake (packageRoot, take, toRate, media); ! converted.has_value())
            return failed (converted.error());
        ++progress.filesDone;
    }
    for (const auto& backingTrack : song.backingTracks)
    {
        if (stop.stop_requested())
            return cancelled();
        auto converted = convertBackingTrack (packageRoot, backingTrack, toRate);
        if (! converted.has_value())
            return failed (converted.error());
        if (converted->has_value())
            media.backingTracks[backingTrack.id] = **converted;
        ++progress.filesDone;
    }
    return media;
}

void deleteConvertedMedia (const juce::File& packageRoot, const ConvertedMedia& media)
{
    for (const auto& [replacedId, take] : media.takes)
        for (const auto& file : { packageRoot.getChildFile (take.file), peaksFileFor (packageRoot, take.id) })
        {
            file.deleteFile();
            tempFileFor (file).deleteFile();
        }
    for (const auto& [replacedId, backingTrack] : media.backingTracks)
        deleteBackingTrackFiles (packageRoot, backingTrack);
}

//==============================================================================

core::Expected<std::unique_ptr<SampleRateConversionRun>> SampleRateConversionRun::start (SongPackage& package, int toRate, Clock::time_point now)
{
    if (toRate <= 0)
        return std::unexpected (core::Error { "song.sample_rate_invalid", "a sample rate must be positive", { { "sample_rate", toRate } } });
    if (toRate == package.song().sampleRate)
        return std::unexpected (core::Error { "song.sample_rate_unchanged", "the Song is at this sample rate already", { { "sample_rate", toRate } } });

    std::unique_ptr<SampleRateConversionRun> run (new SampleRateConversionRun (package, toRate, now));
    run->progress.files = (int) (package.song().takes.size() + package.song().backingTracks.size());
    core::logEvent (core::Level::info, "song.sample_rate_conversion_started", run->fields());
    run->worker = std::jthread ([target = run.get(), root = package.location(), song = package.song(), toRate] (std::stop_token stop) {
        target->outcome = convertMediaFiles (root, song, toRate, target->progress, stop);
        target->done.store (true, std::memory_order_release);
    });
    return run;
}

SampleRateConversionRun::SampleRateConversionRun (SongPackage& songPackage, int toRate, Clock::time_point now)
    : package (songPackage),
      from (songPackage.song().sampleRate),
      to (toRate),
      startedAt (now)
{
}

SampleRateConversionRun::~SampleRateConversionRun()
{
    cancel (Clock::now());
}

double SampleRateConversionRun::fraction() const
{
    const auto files = progress.files.load();
    return files > 0 ? (double) progress.filesDone.load() / (double) files : 1.0;
}

core::Fields SampleRateConversionRun::fields() const
{
    return { { "from_rate", from }, { "to_rate", to }, { "files", progress.files.load() } };
}

std::optional<core::Expected<void>> SampleRateConversionRun::advance (Clock::time_point now)
{
    if (over || ! done.load (std::memory_order_acquire))
        return std::nullopt;
    worker.join();
    over = true;
    if (! outcome->has_value())
        return std::unexpected (outcome->error());
    auto applied = package.applySampleRateConversion (to, **outcome, now);
    auto logged = fields();
    logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
    if (package.song().sampleRate == to)
        core::logEvent (core::Level::info, "song.sample_rate_converted", logged);
    return applied;
}

void SampleRateConversionRun::cancel (Clock::time_point now)
{
    if (over)
        return;
    worker.request_stop();
    worker.join();
    over = true;
    if (outcome.has_value() && outcome->has_value())
        deleteConvertedMedia (package.location(), **outcome);
    auto logged = fields();
    logged["files_done"] = progress.filesDone.load();
    logged["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds> (now - startedAt).count();
    core::logEvent (core::Level::info, "song.sample_rate_conversion_cancelled", logged);
}
} // namespace overdub::song
