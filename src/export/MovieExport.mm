#include "export/MovieExport.h"

#include "core/Log.h"
#include "video/MovieFile.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <cmath>
#include <string>

// Compiled with ARC (src/export/CMakeLists.txt).
namespace overdub::exporting
{
namespace
{
constexpr std::int64_t pollIntervalNs = 20'000'000;

std::string describe (NSError* error, const char* fallback)
{
    const char* text = error != nil ? error.localizedDescription.UTF8String : nullptr;
    return text != nullptr ? std::string (text) : std::string (fallback);
}

NSURL* urlFor (const juce::File& file)
{
    NSString* path = [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
    return [NSURL fileURLWithPath:path != nil ? path : @""];
}

AVAssetTrack* firstTrack (AVAsset* asset, AVMediaType type)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The synchronous track list: this runs on the export's worker thread.
    return [asset tracksWithMediaType:type].firstObject;
#pragma clang diagnostic pop
}

core::Error movieFailed (const char* step, const std::string& detail)
{
    return core::Error { "export.movie_failed", "the MP4 could not be made", { { "step", step }, { "detail", detail } } };
}

core::Error unreadableVideoTake (const MovieSource& source, const std::string& reason)
{
    return core::Error {
        "export.video_take_unreadable",
        "the Video Take's movie could not be read",
        { { "path", source.movie.getFullPathName().toStdString() }, { "video_take_id", source.videoTake.id }, { "reason", reason } },
    };
}

// The files the export writes before the MP4 is complete: hidden siblings of the destination.
juce::File mixFileFor (const juce::File& destination)
{
    return destination.getSiblingFile ("." + destination.getFileNameWithoutExtension() + ".mix.wav");
}

juce::File partialFileFor (const juce::File& destination)
{
    return destination.getSiblingFile ("." + destination.getFileNameWithoutExtension() + ".partial.mp4");
}

// Deletes a file when it goes out of scope.
struct DeleteOnExit
{
    juce::File file;
    ~DeleteOnExit() { file.deleteFile(); }
};

CMTime samplesToTime (song::SampleCount samples, int sampleRate)
{
    return CMTimeMake (samples, (int32_t) sampleRate);
}

// Where the Video Take's movie enters the composition (ADR 0003): at its start in Song samples,
// less `leadSamples`, its movie scaled by the drift ratio. Movie before composition time 0 is cut.
core::Expected<AVMutableCompositionTrack*> composeVideo (AVMutableComposition* composition, AVAssetTrack* source, const song::VideoTake& videoTake, song::SampleCount leadSamples, int sampleRate)
{
    const auto ratio = videoTake.songSamplesPerMovieSecond;
    const auto compositionStart = videoTake.startSample - leadSamples;
    const auto skipSeconds = compositionStart < 0 ? (double) -compositionStart / ratio : 0.0;
    const auto sourceRange = source.timeRange;
    const auto skip = CMTimeMakeWithSeconds (skipSeconds, std::max<int32_t> (sourceRange.duration.timescale, 1'000'000));
    if (CMTimeCompare (skip, sourceRange.duration) >= 0)
        return std::unexpected (movieFailed ("compose_video", "the Video Take ends before the Song starts"));
    const auto inserted = CMTimeRangeMake (CMTimeAdd (sourceRange.start, skip), CMTimeSubtract (sourceRange.duration, skip));
    const auto insertAt = samplesToTime (std::max<song::SampleCount> (0, compositionStart), sampleRate);

    auto* track = [composition addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
    track.preferredTransform = source.preferredTransform;
    NSError* error = nil;
    if (! [track insertTimeRange:inserted ofTrack:source atTime:insertAt error:&error])
        return std::unexpected (movieFailed ("compose_video", describe (error, "insertTimeRange failed")));
    const auto scaledSamples = (song::SampleCount) std::llround (CMTimeGetSeconds (inserted.duration) * ratio);
    [track scaleTimeRange:CMTimeRangeMake (insertAt, inserted.duration) toDuration:samplesToTime (scaledSamples, sampleRate)];
    return track;
}

// The mix file at time 0. It already runs to the range's end (writeMixFile pads it with silence).
core::Expected<void> composeAudio (AVMutableComposition* composition, const juce::File& mixFile)
{
    auto* asset = [AVURLAsset URLAssetWithURL:urlFor (mixFile) options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
    auto* source = firstTrack (asset, AVMediaTypeAudio);
    if (source == nil)
        return std::unexpected (movieFailed ("compose_audio", "the rendered mix has no audio track"));
    auto* track = [composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
    NSError* error = nil;
    if (! [track insertTimeRange:source.timeRange ofTrack:source atTime:kCMTimeZero error:&error])
        return std::unexpected (movieFailed ("compose_audio", describe (error, "insertTimeRange failed")));
    return {};
}

// The export session ends the video track where its last segment ends, empty or not: after the
// movie, its first frame is stretched to `end` and hidden (opacity 0 in the video composition), so
// the black background shows there. Returns where the movie ends.
core::Expected<CMTime> padAfterMovie (AVMutableCompositionTrack* track, AVAssetTrack* source, CMTime end)
{
    const auto movieEnd = CMTimeRangeGetEnd (track.timeRange);
    if (CMTimeCompare (movieEnd, end) >= 0)
        return movieEnd;
    const auto oneFrame = CMTimeRangeMake (source.timeRange.start, CMTimeMinimum (source.timeRange.duration, source.minFrameDuration));
    NSError* error = nil;
    if (! [track insertTimeRange:oneFrame ofTrack:source atTime:movieEnd error:&error])
        return std::unexpected (movieFailed ("compose_video", describe (error, "insertTimeRange failed")));
    [track scaleTimeRange:CMTimeRangeMake (movieEnd, oneFrame.duration) toDuration:CMTimeSubtract (end, movieEnd)];
    return movieEnd;
}

// Black wherever the Video Take does not play (before `movieStart`, from `movieEnd`), its frames
// re-timed onto a grid of `frameSeconds`.
AVMutableVideoComposition* blackBackgroundComposition (AVMutableComposition* composition, AVMutableCompositionTrack* videoTrack, AVAssetTrack* source, CMTime movieEnd, double frameSeconds, int sampleRate)
{
    auto* layer = [AVMutableVideoCompositionLayerInstruction videoCompositionLayerInstructionWithAssetTrack:videoTrack];
    [layer setTransform:source.preferredTransform atTime:kCMTimeZero];
    [layer setOpacity:0.0f atTime:movieEnd];
    auto* instruction = [AVMutableVideoCompositionInstruction videoCompositionInstruction];
    instruction.timeRange = CMTimeRangeMake (kCMTimeZero, composition.duration);
    instruction.backgroundColor = CGColorGetConstantColor (kCGColorBlack);
    instruction.layerInstructions = @[ layer ];

    auto* videoComposition = [AVMutableVideoComposition videoComposition];
    const auto gridTimescale = (int32_t) sampleRate * 1000;
    videoComposition.frameDuration = CMTimeMake (std::max<std::int64_t> (1, std::llround (frameSeconds * gridTimescale)), gridTimescale);
    const auto size = CGSizeApplyAffineTransform (source.naturalSize, source.preferredTransform);
    videoComposition.renderSize = CGSizeMake (std::abs (size.width), std::abs (size.height));
    videoComposition.instructions = @[ instruction ];
    return videoComposition;
}

// Runs the export session to completion, reporting its progress; a stop cancels it.
core::Expected<void> encode (AVAssetExportSession* session, const ProgressFn& progress, const ShouldStopFn& shouldStop)
{
    auto finished = dispatch_semaphore_create (0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [session exportAsynchronouslyWithCompletionHandler:^{
      dispatch_semaphore_signal (finished);
    }];
    bool cancelled = false;
    while (dispatch_semaphore_wait (finished, dispatch_time (DISPATCH_TIME_NOW, pollIntervalNs)) != 0)
    {
        if (! cancelled && shouldStop && shouldStop())
        {
            [session cancelExport];
            cancelled = true;
        }
        if (progress)
            progress (0.5 + 0.5 * std::clamp ((double) session.progress, 0.0, 1.0));
    }
    const auto status = session.status;
    NSError* error = session.error;
#pragma clang diagnostic pop
    if (cancelled || status == AVAssetExportSessionStatusCancelled)
        return std::unexpected (core::Error { "export.cancelled", "the export was stopped", {} });
    if (status != AVAssetExportSessionStatusCompleted)
        return std::unexpected (movieFailed ("encode", describe (error, "the export session did not complete")));
    return {};
}
} // namespace

core::Expected<ExportResult> exportMovie (MixRender& render, const ExportRequest& request, const MovieSource& source, const ProgressFn& progress, const ShouldStopFn& shouldStop)
{
    const auto timing = video::readMovieTiming (source.movie);
    if (! timing.has_value())
        return std::unexpected (unreadableVideoTake (source, timing.error().fields.value ("reason", "unreadable")));
    if (timing->frameSeconds.empty() || source.videoTake.songSamplesPerMovieSecond <= 0.0)
        return std::unexpected (unreadableVideoTake (source, "no frames"));

    const auto sampleRate = render.sampleRate();
    const auto videoSpan = SampleRange { source.videoTake.startSample, video::videoTakeEnd (source.videoTake, *timing) };
    const auto range = movieExportRange (request.videoLength, render.totalSamples(), videoSpan);
    if (range.length() <= 0)
        return std::unexpected (core::Error { "export.video_take_outside_song",
                                              "the Video Take ends before the Song starts",
                                              { { "video_take_id", source.videoTake.id }, { "video_take_end_sample", videoSpan.end } } });

    if (auto folder = createExportFolder (request.destination); ! folder.has_value())
        return std::unexpected (folder.error());

    const auto path = request.destination.getFullPathName().toStdString();
    core::logEvent (core::Level::info,
                    "export.started",
                    { { "format", formatName (ExportFormat::mp4) },
                      { "path", path },
                      { "video_take_id", source.videoTake.id },
                      { "length", videoLengthName (request.videoLength) },
                      { "start_sample", range.start },
                      { "end_sample", range.end },
                      { "sample_rate", sampleRate } });
    const auto startNs = core::hostTimeNowNs();

    // The mix: the first half of the progress.
    const DeleteOnExit mixFile { mixFileFor (request.destination) };
    const auto halfProgress = [&progress] (double fraction) {
        if (progress)
            progress (0.5 * fraction);
    };
    // Padded with silence to the range's end: a Video Take may outlast the mix, or the Song may
    // have no Clips at all (an empty mix).
    if (auto mixed = writeMixFile (render, ExportFormat::wav, mixFile.file, halfProgress, shouldStop, range.end); ! mixed.has_value())
        return std::unexpected (mixed.error());

    @autoreleasepool
    {
        auto* movieAsset = [AVURLAsset URLAssetWithURL:urlFor (source.movie) options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
        auto* sourceVideo = firstTrack (movieAsset, AVMediaTypeVideo);
        if (sourceVideo == nil)
            return std::unexpected (unreadableVideoTake (source, "no video track"));

        const auto fullSong = request.videoLength == VideoLength::fullSong;
        // The full song's output grid: the Video Take's mean frame on the Song clock.
        const auto meanFrameMovieSeconds = timing->durationSeconds / (double) timing->frameSeconds.size();
        const auto gridSeconds = meanFrameMovieSeconds * source.videoTake.songSamplesPerMovieSecond / sampleRate;
        const auto leadSamples = fullSong ? (song::SampleCount) std::llround (0.5 * gridSeconds * sampleRate) : 0;

        auto* composition = [AVMutableComposition composition];
        if (auto audio = composeAudio (composition, mixFile.file); ! audio.has_value())
            return std::unexpected (audio.error());
        auto videoTrack = composeVideo (composition, sourceVideo, source.videoTake, leadSamples, sampleRate);
        if (! videoTrack.has_value())
            return std::unexpected (videoTrack.error());
        AVMutableVideoComposition* blackOutside = nil;
        if (fullSong)
        {
            const auto movieEnd = padAfterMovie (*videoTrack, sourceVideo, samplesToTime (range.end, sampleRate));
            if (! movieEnd.has_value())
                return std::unexpected (movieEnd.error());
            blackOutside = blackBackgroundComposition (composition, *videoTrack, sourceVideo, *movieEnd, gridSeconds, sampleRate);
        }

        const DeleteOnExit partial { partialFileFor (request.destination) };
        partial.file.deleteFile();
        auto* session = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
        if (session == nil)
            return std::unexpected (movieFailed ("encode", "no export session for the composition"));
        session.outputFileType = AVFileTypeMPEG4;
        session.outputURL = urlFor (partial.file);
        session.timeRange = CMTimeRangeMake (samplesToTime (range.start, sampleRate), samplesToTime (range.length(), sampleRate));
        session.videoComposition = blackOutside; // nil when trimmed: the frames keep their own times

        if (auto encoded = encode (session, progress, shouldStop); ! encoded.has_value())
            return std::unexpected (encoded.error());
        if (auto moved = moveExportIntoPlace (partial.file, request.destination); ! moved.has_value())
            return std::unexpected (moved.error());
    }

    if (progress)
        progress (1.0);
    const auto elapsedSeconds = (double) (core::hostTimeNowNs() - startNs) / 1.0e9;
    core::logEvent (core::Level::info,
                    "export.finished",
                    { { "format", formatName (ExportFormat::mp4) }, { "path", path }, { "samples", range.length() }, { "elapsed_ms", elapsedSeconds * 1000.0 } });
    return ExportResult { request.destination, range.length() };
}
} // namespace overdub::exporting
