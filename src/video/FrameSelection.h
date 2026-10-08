#pragma once

#include "engine/Anchor.h"
#include "song/Song.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Video Take playback on the audio clock (ADR 0003, issue #36). Pure functions, no AVFoundation.
//
// Playback pulls frames: on each display refresh the Song sample heard then is
//     latest anchor's Song sample + (now - anchor host time + one refresh) * rate - output latency
// (the anchor's host time excludes the output latency, ADR 0003 amendment). That sample picks the
// Video Take covering it (the newest Pass's on overlap), the sample becomes movie time through the
// Video Take's start and drift ratio, and the frame shown is the last one at or before that movie
// time: a camera stall holds the frame before the gap. The video never runs a clock of its own.
namespace overdub::video
{
using song::SampleCount;

// What the engine says about the transport now (OpenSong::playbackClock).
struct PlaybackClock
{
    bool playing = false;
    SampleCount position = 0;            // Engine::position(): the playhead while stopped
    std::optional<engine::Anchor> anchor; // the latest callback's; none before the first
    int outputLatencySamples = 0;         // the device's reported output latency
    int sampleRate = 48000;
};

// The Song sample (fractional) heard at the display refresh after `nowHostNs`, `refreshNs` from
// now. While playing, from the latest anchor as above; while stopped, or playing before the first
// anchor, the position (a seek or scrub shows its frame at once). When the audio stalls (no anchor
// for longer than maxExtrapolationNs) the picture stops with it, at the anchor plus that long.
inline constexpr std::uint64_t maxExtrapolationNs = 250'000'000;
double heardSongSample (const PlaybackClock& clock, std::uint64_t nowHostNs, std::uint64_t refreshNs);

// A movie's frame times, read from its file (video/MovieFile.h).
struct MovieTiming
{
    std::vector<double> frameSeconds; // movie time of each frame, ascending, from 0 (the first frame)
    double durationSeconds = 0.0;     // to the end of the last frame, taken as long as the mean one

    bool operator== (const MovieTiming&) const = default;
};

// The timing of a Video Take's movie; null when its movie cannot be read.
using TimingLookup = std::function<const MovieTiming*(const song::VideoTake&)>;

// Where a Video Take plays on the timeline: [start, end) in Song samples.
struct VideoTakeSpan
{
    song::Id videoTakeId;
    std::string title;   // "Pass 2"
    SampleCount start = 0;
    SampleCount end = 0; // start when its movie cannot be read: it covers nothing
    bool readable = true;

    bool operator== (const VideoTakeSpan&) const = default;
};

// Where a Video Take's movie ends on the Song clock: its start plus the movie's duration through
// the drift ratio, rounded to the nearest sample.
SampleCount videoTakeEnd (const song::VideoTake& videoTake, const MovieTiming& timing);

// One span per Video Take of the Song, in the Song's order.
std::vector<VideoTakeSpan> videoTakeSpans (const song::Song& song, const TimingLookup& timingOf);

// The movie time (seconds) at which this Song sample plays in the Video Take.
double movieSecondsAt (const song::VideoTake& videoTake, double songSample);

// The frame shown at this movie time: the last one at or before it (held across a gap and after
// the last frame); none before the first frame or without frames.
std::optional<std::size_t> frameIndexAt (const std::vector<double>& frameSeconds, double movieSeconds);

// What a screen shows for a heard Song sample. Equality compares stored values exactly.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
struct FrameChoice
{
    std::optional<song::Id> videoTakeId; // none: black, "No Video Take at the playhead"
    std::optional<std::size_t> frameIndex;
    double movieSeconds = 0.0;

    bool operator== (const FrameChoice&) const = default;
};
#pragma clang diagnostic pop

// The Video Take covering the heard sample (the one whose Pass is newest, by the Song's Pass
// order, when several do) and its frame there.
FrameChoice chooseFrame (const song::Song& song, const TimingLookup& timingOf, double heardSample);
} // namespace overdub::video
