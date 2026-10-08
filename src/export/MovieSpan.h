#pragma once

#include "song/Song.h"

// What part of the Song an MP4 export covers (issue #38, ADR 0003). Pure; no I/O.
namespace overdub::exporting
{
// The MP4's length: trimmed to the chosen Video Take, or the full song with black where there is
// no video.
enum class VideoLength
{
    trimToVideoTake,
    fullSong
};

// [start, end) in Song samples.
struct SampleRange
{
    song::SampleCount start = 0;
    song::SampleCount end = 0;

    song::SampleCount length() const { return end - start; }
    bool operator== (const SampleRange&) const = default;
};

// "trim", "full": the event field value.
const char* videoLengthName (VideoLength length);

// The Song span the MP4 covers. `mixEnd`: the end of the mix (what the audio export renders);
// `videoTake`: where the Video Take plays (start, end of its movie). Trimmed: the Video Take's
// span; full song: from 0 to the later of the mix's end and the Video Take's end. Nothing before
// Song sample 0 is exported.
SampleRange movieExportRange (VideoLength length, song::SampleCount mixEnd, SampleRange videoTake);
} // namespace overdub::exporting
