#include "video/MovieFile.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// Compiled with ARC (src/video/CMakeLists.txt).
namespace overdub::video
{
namespace
{
// A decoded frame this close to the frame time asked for is that frame.
constexpr double sameFrameSeconds = 1.0e-6;
// A restarted reader starts this much before the frame asked for, so it returns that frame.
constexpr double restartLeadSeconds = 0.001;
// The duration given to a movie of one frame.
constexpr double singleFrameSeconds = 1.0 / 30.0;

std::string describe (NSError* error, const char* fallback)
{
    const char* text = error != nil ? error.localizedDescription.UTF8String : nullptr;
    return text != nullptr ? std::string (text) : std::string (fallback);
}

core::Error unreadable (const juce::File& movie, const std::string& reason)
{
    return core::Error { "video.movie_unreadable", "the movie could not be read", { { "path", movie.getFullPathName().toStdString() }, { "reason", reason } } };
}

struct OpenedTrack
{
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
};

core::Expected<OpenedTrack> openVideoTrack (const juce::File& movie)
{
    if (! movie.existsAsFile())
        return std::unexpected (unreadable (movie, "no such file"));
    NSString* path = [NSString stringWithUTF8String:movie.getFullPathName().toRawUTF8()];
    auto* url = [NSURL fileURLWithPath:path != nil ? path : @""];
    auto* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The synchronous track list: this runs on the message thread, once per movie.
    auto* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
#pragma clang diagnostic pop
    if (track == nil)
        return std::unexpected (unreadable (movie, "no video track"));
    return OpenedTrack { asset, track };
}
} // namespace

core::Expected<MovieTiming> readMovieTiming (const juce::File& movie)
{
    auto opened = openVideoTrack (movie);
    if (! opened.has_value())
        return std::unexpected (opened.error());

    NSError* error = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:opened->asset error:&error];
    if (reader == nil)
        return std::unexpected (unreadable (movie, describe (error, "no reader")));
    // No output settings: the compressed samples, so no picture is decoded.
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:opened->track outputSettings:nil];
    output.alwaysCopiesSampleData = NO;
    [reader addOutput:output];
    if (! [reader startReading])
        return std::unexpected (unreadable (movie, describe (reader.error, "startReading failed")));

    std::vector<CMTime> times;
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer])
    {
        if (CMSampleBufferGetNumSamples (sample) > 0)
            times.push_back (CMSampleBufferGetPresentationTimeStamp (sample));
        CFRelease (sample);
    }
    if (reader.status == AVAssetReaderStatusFailed)
        return std::unexpected (unreadable (movie, describe (reader.error, "reading failed")));
    if (times.empty())
        return std::unexpected (unreadable (movie, "no frames"));

    // Compressed samples come in decode order.
    std::ranges::sort (times, [] (CMTime a, CMTime b) { return CMTimeCompare (a, b) < 0; });
    MovieTiming timing;
    timing.frameSeconds.reserve (times.size());
    for (const auto time : times)
        timing.frameSeconds.push_back (CMTimeGetSeconds (CMTimeSubtract (time, times.front())));
    const auto span = timing.frameSeconds.back();
    const auto meanFrame = times.size() > 1 ? span / (double) (times.size() - 1) : singleFrameSeconds;
    timing.durationSeconds = span + meanFrame;
    return timing;
}

struct FrameDecoder::Native
{
    juce::File movie;
    MovieTiming timing;
    AVURLAsset* asset = nil;
    AVAssetTrack* track = nil;
    CMTime firstPts = kCMTimeZero;
    AVAssetReader* reader = nil;
    AVAssetReaderTrackOutput* output = nil;
    CMSampleBufferRef current = nullptr; // retained
    std::optional<double> currentSeconds;

    ~Native() { setCurrent (nullptr, std::nullopt); }

    void setCurrent (CMSampleBufferRef sample, std::optional<double> seconds)
    {
        if (current != nullptr)
            CFRelease (current);
        current = sample;
        currentSeconds = seconds;
    }

    // Starts a reader at this movie time (the whole movie from the start for 0).
    core::Expected<void> startReader (double fromSeconds, const char* failureCode)
    {
        [reader cancelReading];
        reader = nil;
        output = nil;
        NSError* error = nil;
        auto* started = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (started == nil)
            return std::unexpected (failure (failureCode, fromSeconds, describe (error, "no reader")));
        if (fromSeconds > 0.0)
            started.timeRange = CMTimeRangeMake (CMTimeAdd (firstPts, CMTimeMakeWithSeconds (fromSeconds, 1'000'000'000)), kCMTimePositiveInfinity);
        NSDictionary* settings = @{
            (id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id) kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        auto* trackOutput = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
        trackOutput.alwaysCopiesSampleData = NO;
        [started addOutput:trackOutput];
        if (! [started startReading])
            return std::unexpected (failure (failureCode, fromSeconds, describe (started.error, "startReading failed")));
        reader = started;
        output = trackOutput;
        return {};
    }

    core::Error failure (const char* code, double movieSeconds, const std::string& reason) const
    {
        const auto path = movie.getFullPathName().toStdString();
        if (std::string_view (code) == "video.movie_unreadable")
            return unreadable (movie, reason);
        return core::Error { code, "the movie's frame could not be decoded", { { "path", path }, { "movie_seconds", movieSeconds }, { "reason", reason } } };
    }
};

FrameDecoder::FrameDecoder (std::unique_ptr<Native> nativeDecoder)
    : native (std::move (nativeDecoder))
{
}

FrameDecoder::~FrameDecoder()
{
    [native->reader cancelReading];
}

core::Expected<std::unique_ptr<FrameDecoder>> FrameDecoder::open (const juce::File& movie, MovieTiming timing)
{
    auto opened = openVideoTrack (movie);
    if (! opened.has_value())
        return std::unexpected (opened.error());
    if (timing.frameSeconds.empty())
        return std::unexpected (unreadable (movie, "no frames"));

    auto native = std::make_unique<Native>();
    native->movie = movie;
    native->timing = std::move (timing);
    native->asset = opened->asset;
    native->track = opened->track;
    if (auto started = native->startReader (0.0, "video.movie_unreadable"); ! started.has_value())
        return std::unexpected (started.error());

    // The first decoded frame fixes movie time 0, and is the current frame.
    CMSampleBufferRef first = [native->output copyNextSampleBuffer];
    if (first == nullptr)
        return std::unexpected (unreadable (movie, describe (native->reader.error, "no frames")));
    native->setCurrent (first, 0.0);
    return std::unique_ptr<FrameDecoder> (new FrameDecoder (std::move (native)));
}

core::Expected<DecodedFrame> FrameDecoder::decode (double movieSeconds)
{
    auto& n = *native;
    const auto index = frameIndexAt (n.timing.frameSeconds, movieSeconds).value_or (0);
    const auto target = n.timing.frameSeconds[index];

    if (n.currentSeconds.has_value() && std::abs (*n.currentSeconds - target) < sameFrameSeconds)
        return DecodedFrame { *n.currentSeconds, n.current, false };

    const auto restart = ! n.currentSeconds.has_value()
                      || target < *n.currentSeconds
                      || index > frameIndexAt (n.timing.frameSeconds, *n.currentSeconds).value_or (0) + seekAheadFrames
                      || n.reader.status != AVAssetReaderStatusReading;
    if (restart)
    {
        n.setCurrent (nullptr, std::nullopt);
        if (auto started = n.startReader (std::max (0.0, target - restartLeadSeconds), "video.frame_unavailable"); ! started.has_value())
            return std::unexpected (started.error());
    }

    while (CMSampleBufferRef sample = [n.output copyNextSampleBuffer])
    {
        if (CMSampleBufferGetImageBuffer (sample) == nullptr)
        {
            CFRelease (sample);
            continue;
        }
        const auto seconds = CMTimeGetSeconds (CMTimeSubtract (CMSampleBufferGetPresentationTimeStamp (sample), n.firstPts));
        n.setCurrent (sample, seconds);
        if (seconds > target - sameFrameSeconds)
            return DecodedFrame { seconds, sample, restart };
    }
    const auto reason = n.reader.status == AVAssetReaderStatusFailed ? describe (n.reader.error, "reading failed") : std::string ("past the last frame");
    if (n.currentSeconds.has_value())
        return DecodedFrame { *n.currentSeconds, n.current, restart };
    return std::unexpected (n.failure ("video.frame_unavailable", movieSeconds, reason));
}
} // namespace overdub::video
