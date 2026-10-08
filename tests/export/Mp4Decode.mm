#include "export/Mp4Decode.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <cstring>

// Compiled with ARC (tests/CMakeLists.txt).
namespace overdub::exporting::testing
{
namespace
{
AVURLAsset* assetFor (const juce::File& movie)
{
    NSString* path = [NSString stringWithUTF8String:movie.getFullPathName().toRawUTF8()];
    return [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
}

AVAssetTrack* firstTrack (AVURLAsset* asset, AVMediaType type)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [asset tracksWithMediaType:type].firstObject;
#pragma clang diagnostic pop
}

double meanLumaOf (CVPixelBufferRef pixels)
{
    CVPixelBufferLockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);
    const auto width = CVPixelBufferGetWidth (pixels);
    const auto height = CVPixelBufferGetHeight (pixels);
    const auto rowBytes = CVPixelBufferGetBytesPerRow (pixels);
    const auto* base = (const std::uint8_t*) CVPixelBufferGetBaseAddress (pixels);
    double sum = 0.0;
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
        {
            const auto* pixel = base + y * rowBytes + x * 4; // B G R A
            sum += 0.114 * pixel[0] + 0.587 * pixel[1] + 0.299 * pixel[2];
        }
    CVPixelBufferUnlockBaseAddress (pixels, kCVPixelBufferLock_ReadOnly);
    return width * height > 0 ? sum / (double) (width * height) : 0.0;
}
} // namespace

std::vector<DecodedVideoFrame> decodeVideoFrames (const juce::File& movie)
{
    auto* asset = assetFor (movie);
    auto* track = firstTrack (asset, AVMediaTypeVideo);
    if (track == nil)
        return {};
    NSError* error = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (reader == nil)
        return {};
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:@{(id) kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)}];
    [reader addOutput:output];
    if (! [reader startReading])
        return {};

    std::vector<DecodedVideoFrame> frames;
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer])
    {
        if (auto* pixels = CMSampleBufferGetImageBuffer (sample))
            frames.push_back ({ CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sample)), meanLumaOf (pixels) });
        CFRelease (sample);
    }
    return frames;
}

juce::AudioBuffer<float> decodeAudio (const juce::File& movie, int sampleRate)
{
    auto* asset = assetFor (movie);
    auto* track = firstTrack (asset, AVMediaTypeAudio);
    if (track == nil)
        return {};
    NSError* error = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (reader == nil)
        return {};
    constexpr int channels = 2;
    NSDictionary* settings = @{
        AVFormatIDKey : @(kAudioFormatLinearPCM),
        AVSampleRateKey : @(sampleRate),
        AVNumberOfChannelsKey : @(channels),
        AVLinearPCMBitDepthKey : @32,
        AVLinearPCMIsFloatKey : @YES,
        AVLinearPCMIsNonInterleaved : @NO,
        AVLinearPCMIsBigEndianKey : @NO,
    };
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
    [reader addOutput:output];
    if (! [reader startReading])
        return {};

    std::vector<float> interleaved;
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer])
    {
        if (auto* block = CMSampleBufferGetDataBuffer (sample))
        {
            const auto bytes = CMBlockBufferGetDataLength (block);
            const auto offset = interleaved.size();
            interleaved.resize (offset + bytes / sizeof (float));
            CMBlockBufferCopyDataBytes (block, 0, bytes, interleaved.data() + offset);
        }
        CFRelease (sample);
    }
    const auto frames = (int) (interleaved.size() / channels);
    juce::AudioBuffer<float> audio (channels, frames);
    for (int i = 0; i < frames; ++i)
        for (int channel = 0; channel < channels; ++channel)
            audio.setSample (channel, i, interleaved[(std::size_t) (i * channels + channel)]);
    return audio;
}

double movieDurationSeconds (const juce::File& movie)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return CMTimeGetSeconds (assetFor (movie).duration);
#pragma clang diagnostic pop
}
} // namespace overdub::exporting::testing
