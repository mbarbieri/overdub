#include "capture/MovieFrames.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cmath>
#include <cstdint>

// Compiled with ARC (src/capture/CMakeLists.txt).
namespace overdub::capture
{
namespace
{
// Luma is sampled on a grid of at most this many points per side: enough to see hands move.
constexpr int gridSize = 64;

core::Error unreadable (const juce::File& movie, const std::string& reason)
{
    return core::Error { "capture.movie_unreadable", "the movie could not be read", { { "path", movie.getFullPathName().toStdString() }, { "reason", reason } } };
}

// Luma (BT.601 weights) of a BGRA frame on a fixed grid.
std::vector<float> lumaGrid (CVPixelBufferRef pixels)
{
    CVPixelBufferLockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);
    const auto width = (int) CVPixelBufferGetWidth (pixels);
    const auto height = (int) CVPixelBufferGetHeight (pixels);
    const auto rowBytes = CVPixelBufferGetBytesPerRow (pixels);
    const auto* base = (const std::uint8_t*) CVPixelBufferGetBaseAddress (pixels);

    std::vector<float> grid;
    grid.reserve ((std::size_t) (gridSize * gridSize));
    for (int gy = 0; gy < gridSize; ++gy)
    {
        const auto y = std::min (height - 1, (gy * height + height / 2) / gridSize);
        for (int gx = 0; gx < gridSize; ++gx)
        {
            const auto x = std::min (width - 1, (gx * width + width / 2) / gridSize);
            const auto* pixel = base + (std::size_t) y * rowBytes + (std::size_t) x * 4; // B G R A
            grid.push_back (0.114f * pixel[0] + 0.587f * pixel[1] + 0.299f * pixel[2]);
        }
    }
    CVPixelBufferUnlockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);
    return grid;
}

double meanDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size())
        return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        sum += std::abs ((double) a[i] - (double) b[i]);
    return sum / (double) a.size();
}
} // namespace

core::Expected<std::vector<MovieFrame>> readMovieFrames (const juce::File& movie)
{
    if (! movie.existsAsFile())
        return std::unexpected (unreadable (movie, "no such file"));

    NSString* path = [NSString stringWithUTF8String:movie.getFullPathName().toRawUTF8()];
    auto* url = [NSURL fileURLWithPath:path != nil ? path : @""];
    auto* asset = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The synchronous track list: this runs off the audio thread, in a selftest or a test.
    auto* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
#pragma clang diagnostic pop
    if (track == nil)
        return std::unexpected (unreadable (movie, "no video track"));

    NSError* error = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (reader == nil)
        return std::unexpected (unreadable (movie, error != nil ? error.localizedDescription.UTF8String : "no reader"));
    NSDictionary* settings = @{(id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
    [reader addOutput:output];
    if (! [reader startReading])
        return std::unexpected (unreadable (movie, reader.error != nil ? reader.error.localizedDescription.UTF8String : "startReading failed"));

    std::vector<MovieFrame> frames;
    std::vector<float> previous;
    std::optional<CMTime> first;
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer])
    {
        const auto pts = CMSampleBufferGetPresentationTimeStamp (sample);
        if (auto* pixels = CMSampleBufferGetImageBuffer (sample))
        {
            if (! first.has_value())
                first = pts;
            auto luma = lumaGrid (pixels);
            frames.push_back (MovieFrame { CMTimeGetSeconds (CMTimeSubtract (pts, *first)), meanDifference (previous, luma) });
            previous = std::move (luma);
        }
        CFRelease (sample);
    }
    if (reader.status == AVAssetReaderStatusFailed)
        return std::unexpected (unreadable (movie, reader.error != nil ? reader.error.localizedDescription.UTF8String : "reading failed"));
    return frames;
}
} // namespace overdub::capture
