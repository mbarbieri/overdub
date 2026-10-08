#pragma once

#include "core/Error.h"

#include <juce_graphics/juce_graphics.h>

#include <cstdint>
#include <memory>
#include <optional>

// One H.264 QuickTime movie written through AVAssetWriter (ADR 0003). The camera appends the
// sample buffers it captures; tests and selftests append images with timestamps they choose.
// Movie time 0 is the first frame: the writer's session starts at the first frame's PTS, and every
// frame keeps its own PTS (a stall stays a gap; nothing is re-timed).
//
// Threads: one thread at a time (the camera's `video` queue while capturing). finish() blocks
// until AVFoundation has written the file.
namespace overdub::capture
{
// What was written, from the frames' own timestamps.
struct MovieWriterStats
{
    std::int64_t frames = 0;
    std::int64_t refusedFrames = 0; // the writer was not ready, or refused the frame
    std::optional<std::int64_t> sessionStartPtsNs; // movie time 0: the PTS the writer's session started at
    std::optional<std::int64_t> firstPtsNs;        // the first written frame's PTS
    std::int64_t lastPtsNs = 0;
    std::int64_t longestGapNs = 0; // the longest interval between two written frames

    // From the first frame to the end of the last, taking the last frame as long as the mean one.
    double durationSeconds() const;
};

class MovieWriter
{
public:
    // Creates (replacing) `file` as a QuickTime movie of this frame size. The extension is free:
    // the recorder writes `<id>.mov.tmp` and renames it.
    // Errors: capture.movie_write_failed (path, reason).
    static core::Expected<std::unique_ptr<MovieWriter>> create (const juce::File& file, int width, int height);
    ~MovieWriter();

    MovieWriter (const MovieWriter&) = delete;
    MovieWriter& operator= (const MovieWriter&) = delete;

    // Appends a CMSampleBufferRef from the camera. Returns false when the frame was not written.
    // The first frame offered starts the movie's session (stats().sessionStartPtsNs), even when
    // the writer then refuses it: movie time 0 is that frame's PTS.
    bool appendSampleBuffer (void* sampleBuffer);
    // Appends an ARGB image (scaled to the movie size if needed) at this presentation time.
    bool appendImage (const juce::Image& image, std::int64_t ptsNs);

    // Ends the movie and waits until the file is complete.
    // Errors: capture.movie_no_frames, capture.movie_write_failed (path, reason).
    core::Expected<MovieWriterStats> finish();

    const MovieWriterStats& stats() const { return written; }
    const juce::File& file() const { return movieFile; }

    struct Native; // AVFoundation objects, in MovieWriter.mm

private:
    MovieWriter (juce::File file, std::unique_ptr<Native> native);

    bool startAt (std::int64_t ptsNs);
    void noteWritten (std::int64_t ptsNs);
    static core::Error failure (const juce::File& file, const std::string& reason);

    juce::File movieFile;
    std::unique_ptr<Native> native;
    MovieWriterStats written;
    bool finished = false;
};
} // namespace overdub::capture
