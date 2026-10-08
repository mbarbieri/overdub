#include "export/MovieSpan.h"

#include <algorithm>

namespace overdub::exporting
{
const char* videoLengthName (VideoLength length)
{
    return length == VideoLength::fullSong ? "full" : "trim";
}

SampleRange movieExportRange (VideoLength length, song::SampleCount mixEnd, SampleRange videoTake)
{
    const auto videoEnd = std::max<song::SampleCount> (0, videoTake.end);
    if (length == VideoLength::fullSong)
        return { 0, std::max (mixEnd, videoEnd) };
    const auto start = std::clamp<song::SampleCount> (videoTake.start, 0, videoEnd);
    return { start, videoEnd };
}
} // namespace overdub::exporting
