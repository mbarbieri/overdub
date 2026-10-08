#include "capture/MovieWriter.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>
#include <dispatch/dispatch.h>

// Compiled with ARC (src/capture/CMakeLists.txt).
namespace overdub::capture
{
namespace
{
constexpr std::int64_t nsPerSecond = 1'000'000'000;
constexpr std::int64_t finishTimeoutNs = 10 * nsPerSecond;

std::string describe (NSError* error)
{
    const char* text = error != nil ? error.localizedDescription.UTF8String : nullptr;
    return text != nullptr ? std::string (text) : std::string ("unknown");
}

NSURL* fileUrl (const juce::File& file)
{
    NSString* path = [NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()];
    return [NSURL fileURLWithPath:path != nil ? path : @""];
}

std::int64_t toNs (CMTime time)
{
    return CMTimeConvertScale (time, (int32_t) nsPerSecond, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}
} // namespace

struct MovieWriter::Native
{
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* input = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    int width = 0;
    int height = 0;
    bool started = false;
};

double MovieWriterStats::durationSeconds() const
{
    if (! firstPtsNs.has_value() || frames == 0)
        return 0.0;
    const auto span = (double) (lastPtsNs - *firstPtsNs);
    const auto meanFrame = frames > 1 ? span / (double) (frames - 1) : 0.0;
    return (span + meanFrame) / (double) nsPerSecond;
}

MovieWriter::MovieWriter (juce::File file, std::unique_ptr<Native> nativeWriter)
    : movieFile (std::move (file)), native (std::move (nativeWriter))
{
}

MovieWriter::~MovieWriter()
{
    if (! finished && native->started)
        [native->writer cancelWriting];
}

core::Error MovieWriter::failure (const std::string& reason) const
{
    return core::Error { "capture.movie_write_failed", "the movie could not be written", { { "path", movieFile.getFullPathName().toStdString() }, { "reason", reason } } };
}

core::Expected<std::unique_ptr<MovieWriter>> MovieWriter::create (const juce::File& file, int width, int height)
{
    file.deleteFile();
    auto native = std::make_unique<Native>();
    native->width = width;
    native->height = height;

    NSError* error = nil;
    native->writer = [[AVAssetWriter alloc] initWithURL:fileUrl (file) fileType:AVFileTypeQuickTimeMovie error:&error];
    const auto fail = [&] (const std::string& reason) {
        return core::Error { "capture.movie_write_failed", "the movie could not be written", { { "path", file.getFullPathName().toStdString() }, { "reason", reason } } };
    };
    if (native->writer == nil)
        return std::unexpected (fail (describe (error)));

    NSDictionary* settings = @{ AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @(width), AVVideoHeightKey: @(height) };
    native->input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
    native->input.expectsMediaDataInRealTime = YES;
    if (! [native->writer canAddInput:native->input])
        return std::unexpected (fail ("the writer refused an H.264 input of this size"));
    [native->writer addInput:native->input];

    NSDictionary* pixelAttributes = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                       (id) kCVPixelBufferWidthKey: @(width),
                                       (id) kCVPixelBufferHeightKey: @(height) };
    native->adaptor = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:native->input
                                                                                       sourcePixelBufferAttributes:pixelAttributes];
    return std::unique_ptr<MovieWriter> (new MovieWriter (file, std::move (native)));
}

bool MovieWriter::startAt (std::int64_t ptsNs)
{
    if (native->started)
        return true;
    if (! [native->writer startWriting])
        return false;
    [native->writer startSessionAtSourceTime:CMTimeMake (ptsNs, (int32_t) nsPerSecond)];
    native->started = true;
    return true;
}

void MovieWriter::noteWritten (std::int64_t ptsNs)
{
    if (written.firstPtsNs.has_value())
        written.longestGapNs = std::max (written.longestGapNs, ptsNs - written.lastPtsNs);
    else
        written.firstPtsNs = ptsNs;
    written.lastPtsNs = ptsNs;
    ++written.frames;
}

bool MovieWriter::appendSampleBuffer (void* sampleBuffer)
{
    auto* buffer = (CMSampleBufferRef) sampleBuffer;
    const auto ptsNs = toNs (CMSampleBufferGetPresentationTimeStamp (buffer));
    if (finished || ! startAt (ptsNs) || ! native->input.readyForMoreMediaData || ! [native->input appendSampleBuffer:buffer])
    {
        ++written.refusedFrames;
        return false;
    }
    noteWritten (ptsNs);
    return true;
}

bool MovieWriter::appendImage (const juce::Image& image, std::int64_t ptsNs)
{
    if (finished || ! startAt (ptsNs))
    {
        ++written.refusedFrames;
        return false;
    }
    // Offline writing: wait for the encoder rather than drop the frame.
    for (int attempt = 0; attempt < 2000 && ! native->input.readyForMoreMediaData; ++attempt)
        [NSThread sleepForTimeInterval:0.001];

    CVPixelBufferRef pixels = nullptr;
    CVPixelBufferPoolRef pool = native->adaptor.pixelBufferPool;
    if (pool == nullptr || CVPixelBufferPoolCreatePixelBuffer (nullptr, (CVPixelBufferPoolRef _Nonnull) pool, &pixels) != kCVReturnSuccess)
    {
        ++written.refusedFrames;
        return false;
    }

    const auto scaled = image.getWidth() == native->width && image.getHeight() == native->height
                          ? image.convertedToFormat (juce::Image::ARGB)
                          : image.rescaled (native->width, native->height).convertedToFormat (juce::Image::ARGB);
    CVPixelBufferLockBaseAddress (pixels, 0);
    auto* base = (std::uint8_t*) CVPixelBufferGetBaseAddress (pixels);
    const auto rowBytes = CVPixelBufferGetBytesPerRow (pixels);
    {
        const juce::Image::BitmapData data (scaled, juce::Image::BitmapData::readOnly);
        // JUCE ARGB is BGRA in memory on little-endian machines, as the pixel buffer is.
        for (int y = 0; y < native->height; ++y)
            std::memcpy (base + (std::size_t) y * rowBytes, data.getLinePointer (y), (std::size_t) native->width * 4);
    }
    CVPixelBufferUnlockBaseAddress (pixels, 0);

    const auto appended = native->input.readyForMoreMediaData && [native->adaptor appendPixelBuffer:pixels withPresentationTime:CMTimeMake (ptsNs, (int32_t) nsPerSecond)];
    CVPixelBufferRelease (pixels);
    if (! appended)
    {
        ++written.refusedFrames;
        return false;
    }
    noteWritten (ptsNs);
    return true;
}

core::Expected<MovieWriterStats> MovieWriter::finish()
{
    if (finished)
        return std::unexpected (failure ("already finished"));
    finished = true;
    if (! native->started || written.frames == 0)
    {
        if (native->started)
            [native->writer cancelWriting];
        movieFile.deleteFile();
        return std::unexpected (core::Error { "capture.movie_no_frames", "the camera delivered no frames", { { "path", movieFile.getFullPathName().toStdString() } } });
    }

    [native->input markAsFinished];
    auto done = dispatch_semaphore_create (0);
    [native->writer finishWritingWithCompletionHandler:^{
      dispatch_semaphore_signal (done);
    }];
    if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, finishTimeoutNs)) != 0)
        return std::unexpected (failure ("finishing the movie timed out"));
    if (native->writer.status != AVAssetWriterStatusCompleted)
        return std::unexpected (failure (describe (native->writer.error)));
    return written;
}
} // namespace overdub::capture
