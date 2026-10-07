// PROTOTYPE — issue #7 spike.
#include "ClapAnalysis.h"
#include "SpikeLog.h"
#include <juce_graphics/juce_graphics.h>
#import <AVFoundation/AVFoundation.h>

using namespace juce;

namespace
{
constexpr double windowSeconds = 0.3;
constexpr double refractorySeconds = 0.25;
constexpr int thumbWidth = 320;
constexpr int columns = 6;

NSURL* urlFor (const File& file)
{
    return [NSURL fileURLWithPath:[NSString stringWithUTF8String:file.getFullPathName().toRawUTF8()]];
}

// Threshold crossings at 30% of the global peak, at least refractorySeconds apart.
std::vector<int64_t> findClaps (const std::vector<float>& audio, double sampleRate)
{
    float peak = 0;
    for (auto x : audio)
        peak = std::max (peak, std::abs (x));

    std::vector<int64_t> claps;
    const float threshold = std::max (0.3f * peak, 0.01f);
    const auto refractory = (int64_t) (refractorySeconds * sampleRate);
    int64_t last = -refractory;

    for (int64_t i = 0; i < (int64_t) audio.size(); ++i)
    {
        if (std::abs (audio[(size_t) i]) >= threshold && i - last >= refractory)
        {
            claps.push_back (i);
            last = i;
        }
    }
    return claps;
}

struct Frame
{
    double pts = 0;
    Image thumb;
    double motion = 0; // mean |Δgreen| against the previous frame, 0..255
};

Image thumbnailOf (CVPixelBufferRef pixelBuffer)
{
    CVPixelBufferLockBaseAddress (pixelBuffer, kCVPixelBufferLock_ReadOnly);
    const auto width = (int) CVPixelBufferGetWidth (pixelBuffer);
    const auto height = (int) CVPixelBufferGetHeight (pixelBuffer);
    const auto rowBytes = CVPixelBufferGetBytesPerRow (pixelBuffer);
    const auto* base = (const uint8_t*) CVPixelBufferGetBaseAddress (pixelBuffer);

    Image full (Image::ARGB, width, height, false);
    {
        Image::BitmapData data (full, Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
            memcpy (data.getLinePointer (y), base + (size_t) y * rowBytes, (size_t) width * 4); // BGRA == JUCE ARGB in memory
    }
    CVPixelBufferUnlockBaseAddress (pixelBuffer, kCVPixelBufferLock_ReadOnly);

    const int thumbHeight = height * thumbWidth / std::max (1, width);
    return full.rescaled (thumbWidth, thumbHeight, Graphics::mediumResamplingQuality);
}

double motionBetween (const Image& a, const Image& b)
{
    if (! a.isValid() || ! b.isValid() || a.getBounds() != b.getBounds())
        return 0;
    double sum = 0;
    for (int y = 0; y < a.getHeight(); y += 2)
        for (int x = 0; x < a.getWidth(); x += 2)
            sum += std::abs (a.getPixelAt (x, y).getGreen() - b.getPixelAt (x, y).getGreen());
    return sum / ((a.getHeight() / 2) * (a.getWidth() / 2));
}

std::vector<Frame> readFrames (AVAsset* asset, double fromSeconds, double toSeconds)
{
    std::vector<Frame> frames;
    auto* track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (track == nil)
        return frames;

    NSError* error = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    NSDictionary* settings = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
    [reader addOutput:output];
    reader.timeRange = CMTimeRangeFromTimeToTime (CMTimeMakeWithSeconds (std::max (0.0, fromSeconds), 600000),
                                                  CMTimeMakeWithSeconds (toSeconds, 600000));
    if (! [reader startReading])
        return frames;

    Image previous;
    while (auto sample = [output copyNextSampleBuffer])
    {
        Frame frame;
        frame.pts = CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sample));
        if (auto pixelBuffer = CMSampleBufferGetImageBuffer (sample))
            frame.thumb = thumbnailOf (pixelBuffer);
        frame.motion = motionBetween (frame.thumb, previous);
        previous = frame.thumb;
        CFRelease (sample);
        if (frame.pts >= fromSeconds && frame.pts <= toSeconds)
            frames.push_back (std::move (frame));
    }
    return frames;
}

void writeSheet (const File& file, const String& title, const std::vector<Frame>& frames, double clapMovieSeconds)
{
    if (frames.empty())
        return;
    const int cellW = thumbWidth, cellH = frames.front().thumb.getHeight() + 22;
    const int rows = ((int) frames.size() + columns - 1) / columns;
    Image sheet (Image::RGB, cellW * columns, cellH * rows + 26, true);
    Graphics g (sheet);
    g.fillAll (Colours::black);
    g.setColour (Colours::white);
    g.setFont (FontOptions (16.0f));
    g.drawText (title, 6, 0, sheet.getWidth() - 12, 24, Justification::centredLeft);

    for (size_t i = 0; i < frames.size(); ++i)
    {
        const int x = (int) (i % columns) * cellW;
        const int y = 26 + (int) (i / columns) * cellH;
        const auto deltaMs = (frames[i].pts - clapMovieSeconds) * 1000.0;
        const bool containsClap = frames[i].pts <= clapMovieSeconds
                               && (i + 1 == frames.size() || frames[i + 1].pts > clapMovieSeconds);
        g.drawImageAt (frames[i].thumb, x, y);
        g.setColour (containsClap ? Colours::yellow : Colours::white);
        g.drawText ("#" + String ((int) i) + "  d=" + String (deltaMs, 1) + "ms  m=" + String (frames[i].motion, 1),
                    x + 4, y + frames[i].thumb.getHeight(), cellW - 8, 20, Justification::centredLeft);
        if (containsClap)
            g.drawRect (x, y, frames[i].thumb.getWidth(), frames[i].thumb.getHeight(), 3);
    }

    file.deleteFile();
    FileOutputStream out (file);
    PNGImageFormat().writeImageToStream (sheet, out);
}
}

String analyseClaps (const ClapAnalysisInput& in)
{
    in.outputDir.createDirectory();
    auto* asset = [AVURLAsset URLAssetWithURL:urlFor (in.videoFile) options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    const double movieDuration = CMTimeGetSeconds (asset.duration);
    const auto claps = findClaps (in.audio, in.sampleRate);

    String summary;
    summary << "analysis: " << in.label << "\n"
            << "video: " << in.videoFile.getFullPathName() << " (" << String (movieDuration, 3) << " s)\n"
            << "video_start_sample: " << String (in.videoStartSample, 3)
            << "  movie_seconds_per_sample: " << String (in.movieSecondsPerSample * in.sampleRate, 9) << "/rate\n"
            << "claps found: " << (int) claps.size() << "\n"
            << "clap  audio_s      movie_s     clap_frame_d_ms  motion_peak_d_ms  sheet\n";

    int index = 0;
    for (auto clap : claps)
    {
        const double clapMovie = ((double) clap - in.videoStartSample) * in.movieSecondsPerSample;
        const auto audioSeconds = (double) clap / in.sampleRate;
        String line = String (index).paddedLeft (' ', 4) + "  " + String (audioSeconds, 4).paddedRight (' ', 11) + "  "
                    + String (clapMovie, 4).paddedRight (' ', 10) + "  ";

        if (clapMovie < 0 || clapMovie > movieDuration)
        {
            summary << line << "outside video\n";
            ++index;
            continue;
        }

        const auto frames = readFrames (asset, clapMovie - windowSeconds, clapMovie + windowSeconds);
        double clapFrameDelta = 0, motionPeakDelta = 0, motionPeak = -1;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            if (frames[i].pts <= clapMovie)
                clapFrameDelta = (frames[i].pts - clapMovie) * 1000.0;
            if (frames[i].motion > motionPeak)
            {
                motionPeak = frames[i].motion;
                motionPeakDelta = (frames[i].pts - clapMovie) * 1000.0;
            }
        }

        const auto sheetName = "clap-" + String (index).paddedLeft ('0', 2) + ".png";
        writeSheet (in.outputDir.getChildFile (sheetName),
                    in.label + "  clap " + String (index) + " at audio " + String (audioSeconds, 3) + " s",
                    frames, clapMovie);
        summary << line << String (clapFrameDelta, 1).paddedRight (' ', 15) << "  "
                << String (motionPeakDelta, 1).paddedRight (' ', 16) << "  " << sheetName << "\n";
        ++index;
    }

    in.outputDir.getChildFile ("summary.txt").replaceWithText (summary);
    spikelog::line ("clap_analysis", { { "label", in.label }, { "claps", String ((int) claps.size()) },
                                       { "dir", in.outputDir.getFullPathName() } });
    return summary;
}

bool readMovieAudio (const File& movie, double sampleRate, std::vector<float>& audio, double& firstPtsSeconds, String& error)
{
    auto* asset = [AVURLAsset URLAssetWithURL:urlFor (movie) options:@{ AVURLAssetPreferPreciseDurationAndTimingKey: @YES }];
    auto* track = [asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    if (track == nil)
    {
        error = "no audio track in " + movie.getFullPathName();
        return false;
    }

    NSError* nsError = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&nsError];
    NSDictionary* settings = @{ AVFormatIDKey: @(kAudioFormatLinearPCM),
                                AVLinearPCMBitDepthKey: @32,
                                AVLinearPCMIsFloatKey: @YES,
                                AVLinearPCMIsNonInterleaved: @NO,
                                AVLinearPCMIsBigEndianKey: @NO,
                                AVNumberOfChannelsKey: @1,
                                AVSampleRateKey: @(sampleRate) };
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
    [reader addOutput:output];
    if (! [reader startReading])
    {
        error = "audio startReading failed";
        return false;
    }

    audio.clear();
    firstPtsSeconds = -1;
    while (auto sample = [output copyNextSampleBuffer])
    {
        if (firstPtsSeconds < 0)
            firstPtsSeconds = CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sample));
        auto block = CMSampleBufferGetDataBuffer (sample);
        if (block != nullptr)
        {
            const auto length = CMBlockBufferGetDataLength (block);
            const auto start = audio.size();
            audio.resize (start + length / sizeof (float));
            CMBlockBufferCopyDataBytes (block, 0, length, audio.data() + start);
        }
        CFRelease (sample);
    }
    return true;
}
