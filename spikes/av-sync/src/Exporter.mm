// PROTOTYPE — issue #7 spike.
#include "Exporter.h"
#include "SpikeLog.h"
#include <juce_events/juce_events.h>
#import <AVFoundation/AVFoundation.h>

using namespace juce;

namespace
{
NSURL* urlFor (const File& file)
{
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()]];
}

String describe (CMTime t)
{
    return String ((int64) t.value) + "/" + String (t.timescale) + "=" + String (CMTimeGetSeconds (t), 6) + "s";
}

void finish (std::function<void (ExportResult)> onDone, ExportResult result)
{
    spikelog::line ("export_done", { { "ok", result.ok ? "yes" : "no" },
                                     { "file", result.file.getFullPathName() },
                                     { "elapsed_s", String (result.elapsedSeconds, 2) },
                                     { "error", result.error } });
    MessageManager::callAsync ([onDone, result] { onDone (result); });
}
}

void exportSession (const SessionInfo& session, bool fullSong, std::function<void (ExportResult)> onDone)
{
    ExportResult result;
    result.file = session.file (fullSong ? "export-full.mp4" : "export-trimmed.mp4");
    const auto startMs = Time::getMillisecondCounterHiRes();

    auto mixFile = session.file ("mix.wav");
    if (! mixFile.existsAsFile())
    {
        juce::AudioBuffer<float> mix;
        if (! buildSongMix (session, mix, result.error) || ! writeMonoWav (mixFile, mix, session.sampleRate))
        {
            result.error = "mix.wav: " + result.error;
            return finish (onDone, result);
        }
    }

    auto* audioAsset = [AVURLAsset URLAssetWithURL:urlFor (mixFile) options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    auto* videoAsset = [AVURLAsset URLAssetWithURL:urlFor (session.file ("video.mov")) options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    auto* sourceAudio = [audioAsset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    auto* sourceVideo = [videoAsset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (sourceAudio == nil || sourceVideo == nil)
    {
        result.error = "missing source track (audio " + String (sourceAudio != nil ? "ok" : "missing")
                     + ", video " + String (sourceVideo != nil ? "ok" : "missing") + ")";
        return finish (onDone, result);
    }

    // The offset keeps the song's sample timescale: no rounding to frames.
    const auto timescale = (int32_t) std::lround (session.sampleRate);
    const auto videoOffset = CMTimeMake ((int64_t) std::llround (session.videoOffsetSamples), timescale);
    if (CMTimeCompare (videoOffset, kCMTimeZero) < 0)
    {
        result.error = "negative video offset is not handled by the spike";
        return finish (onDone, result);
    }

    auto* composition = [AVMutableComposition composition];
    auto* audioTrack = [composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
    auto* videoTrack = [composition addMutableTrackWithMediaType:AVMediaTypeVideo preferredTrackID:kCMPersistentTrackID_Invalid];
    videoTrack.preferredTransform = sourceVideo.preferredTransform;

    NSError* error = nil;
    const auto audioRange = sourceAudio.timeRange;
    if (! [audioTrack insertTimeRange:audioRange ofTrack:sourceAudio atTime:kCMTimeZero error:&error])
    {
        result.error = "insert audio: " + String (error.localizedDescription.UTF8String);
        return finish (onDone, result);
    }

    const auto videoRange = sourceVideo.timeRange;
    if (! [videoTrack insertTimeRange:videoRange ofTrack:sourceVideo atTime:videoOffset error:&error])
    {
        result.error = "insert video: " + String (error.localizedDescription.UTF8String);
        return finish (onDone, result);
    }

    // Drift: movie seconds are host seconds; song seconds are audio-clock seconds.
    const double songSecondsPerMovieSecond = session.clock.nominalNsPerSample / session.clock.nsPerSample;
    auto videoSongDuration = videoRange.duration;
    if (std::abs (session.clock.ppm()) > 0.5)
    {
        videoSongDuration = CMTimeMultiplyByFloat64 (CMTimeConvertScale (videoRange.duration, timescale, kCMTimeRoundingMethod_RoundHalfAwayFromZero),
                                                     songSecondsPerMovieSecond);
        [videoTrack scaleTimeRange:CMTimeRangeMake (videoOffset, videoRange.duration) toDuration:videoSongDuration];
    }

    spikelog::line ("export_composition", { { "full_song", fullSong ? "yes" : "no" },
                                            { "audio_range", describe (audioRange.start) + "+" + describe (audioRange.duration) },
                                            { "video_range", describe (videoRange.start) + "+" + describe (videoRange.duration) },
                                            { "video_offset", describe (videoOffset) },
                                            { "video_song_duration", describe (videoSongDuration) },
                                            { "clock_ppm", String (session.clock.ppm(), 3) },
                                            { "nominal_fps", String (sourceVideo.nominalFrameRate, 3) } });

    auto* exporter = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetHighestQuality];
    exporter.outputFileType = AVFileTypeMPEG4;
    exporter.outputURL = urlFor (result.file);
    result.file.deleteFile();

    if (fullSong)
    {
        const auto songDuration = composition.duration;
        auto* layer = [AVMutableVideoCompositionLayerInstruction videoCompositionLayerInstructionWithAssetTrack:videoTrack];
        auto* instruction = [AVMutableVideoCompositionInstruction videoCompositionInstruction];
        instruction.timeRange = CMTimeRangeMake (kCMTimeZero, songDuration);
        instruction.backgroundColor = CGColorGetConstantColor (kCGColorBlack);
        instruction.layerInstructions = @[ layer ];

        auto* videoComposition = [AVMutableVideoComposition videoComposition];
        const float fps = sourceVideo.nominalFrameRate > 0 ? sourceVideo.nominalFrameRate : 30.0f;
        videoComposition.frameDuration = CMTimeMake (1000, (int32_t) std::lround (fps * 1000));
        videoComposition.renderSize = sourceVideo.naturalSize;
        videoComposition.instructions = @[ instruction ];
        exporter.videoComposition = videoComposition;
        exporter.timeRange = CMTimeRangeMake (kCMTimeZero, songDuration);
    }
    else
    {
        exporter.timeRange = CMTimeRangeMake (videoOffset, videoSongDuration);
    }

    [exporter exportAsynchronouslyWithCompletionHandler:^{
        ExportResult done = result;
        done.ok = exporter.status == AVAssetExportSessionStatusCompleted;
        done.elapsedSeconds = (Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
        if (! done.ok)
            done.error = "status " + String ((int) exporter.status) + ": "
                       + String (exporter.error != nil ? exporter.error.description.UTF8String : "");
        finish (onDone, done);
    }];
}
