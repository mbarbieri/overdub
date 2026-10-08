#pragma once

#include "song/Song.h"
#include "video/FrameSelection.h"
#include "video/MovieFile.h"

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

// The open Song's Video Take playback (ADR 0003, issue #36): reads each Video Take's movie timing
// once (media never changes), and for a heard Song sample chooses the frame (video/FrameSelection.h)
// and decodes it with one FrameDecoder, kept for the Video Take shown last.
//
// Logs (video area): video.movie_unreadable (warn, once per Video Take: video_take_id, path,
// reason), video.video_take_shown (debug, when the Video Take shown changes: video_take_id, or
// null for none), video.frame_unavailable (warn, once per Video Take).
//
// Message thread only.
namespace overdub::video
{
class VideoPlayback
{
public:
    // Movies are read from the package at `packageRoot` (VideoTake::file is package-relative).
    explicit VideoPlayback (juce::File packageRoot);
    ~VideoPlayback();

    VideoPlayback (const VideoPlayback&) = delete;
    VideoPlayback& operator= (const VideoPlayback&) = delete;

    // The movie's timing, read on first use; null when it cannot be read.
    const MovieTiming* timing (const song::VideoTake& videoTake);
    TimingLookup timingLookup();
    // Where each Video Take of the Song plays (videoTakeSpans).
    std::vector<VideoTakeSpan> spans (const song::Song& song);

    struct Shown
    {
        FrameChoice choice;
        // The frame's CMSampleBufferRef, valid until the next frameAt(); null without a Video Take
        // or when its frame could not be decoded.
        void* sampleBuffer = nullptr;
    };

    // The frame shown for this heard Song sample, decoded.
    Shown frameAt (const song::Song& song, double heardSample);

private:
    void noteShown (const std::optional<song::Id>& videoTakeId);
    FrameDecoder* decoderFor (const song::VideoTake& videoTake, const MovieTiming& timing);

    juce::File root;
    std::map<song::Id, std::optional<MovieTiming>> timings; // none: unreadable (logged once)
    std::optional<song::Id> decoderVideoTakeId;
    std::unique_ptr<FrameDecoder> decoder;
    std::optional<song::Id> lastShown;
    bool anyShown = false;
    std::map<song::Id, bool> decodeFailureLogged;
};
} // namespace overdub::video
