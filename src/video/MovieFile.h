#pragma once

#include "core/Error.h"
#include "video/FrameSelection.h"

#include <juce_core/juce_core.h>

#include <memory>

// A Video Take's movie for playback (ADR 0003, issue #36), read through AVAssetReader.
// Movie time 0 is the first frame's PTS, as the capture side wrote it (capture/MovieWriter.h).
//
// Threads: one thread at a time (the message thread in the app).
namespace overdub::video
{
// The movie's frame times, read without decoding a picture.
// Errors: video.movie_unreadable (path, reason).
core::Expected<MovieTiming> readMovieTiming (const juce::File& movie);

struct DecodedFrame
{
    double seconds = 0.0;          // the frame's movie time
    void* sampleBuffer = nullptr;  // a CMSampleBufferRef with a BGRA CVPixelBuffer, owned by the decoder
    bool restarted = false;        // the reader was restarted to reach it (a seek)
};

// Decodes frames of one movie in order, restarting the reader on a seek.
class FrameDecoder
{
public:
    // `timing` is the movie's (readMovieTiming): a time asked for is snapped to its frame.
    // Errors: video.movie_unreadable (path, reason).
    static core::Expected<std::unique_ptr<FrameDecoder>> open (const juce::File& movie, MovieTiming timing);
    ~FrameDecoder();

    FrameDecoder (const FrameDecoder&) = delete;
    FrameDecoder& operator= (const FrameDecoder&) = delete;

    // The frame shown at this movie time: the last one at or before it (frameIndexAt; the first
    // before the first). Reads forward from the frame decoded last; a step back, or a jump of more than
    // `seekAheadFrames` frames (a camera stall is one frame), restarts the reader there instead. The sample buffer stays valid until the
    // next decode() or the decoder's end.
    // Errors: video.frame_unavailable (path, movie_seconds, reason).
    core::Expected<DecodedFrame> decode (double movieSeconds);

    static constexpr std::size_t seekAheadFrames = 30;

    struct Native; // AVFoundation objects, in MovieFile.mm

private:
    explicit FrameDecoder (std::unique_ptr<Native> native);

    std::unique_ptr<Native> native;
};
} // namespace overdub::video
