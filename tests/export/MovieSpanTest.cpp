#include "export/MovieSpan.h"

#include <catch2/catch_test_macros.hpp>

// The Song span an MP4 export covers (issue #38): trimmed to the Video Take, or the full song
// with black where there is no video. Pure.
using overdub::exporting::movieExportRange;
using overdub::exporting::SampleRange;
using overdub::exporting::VideoLength;

TEST_CASE ("trimmed to the Video Take, the MP4 covers exactly the Video Take's span", "[export][mp4][span]")
{
    CHECK (movieExportRange (VideoLength::trimToVideoTake, 12 * 48000, { 48000, 11 * 48000 }) == SampleRange { 48000, 11 * 48000 });
    // Past the mix's end it keeps going: the audio is silent there.
    CHECK (movieExportRange (VideoLength::trimToVideoTake, 2 * 48000, { 48000, 5 * 48000 }) == SampleRange { 48000, 5 * 48000 });
    // Movie before the Song's start is cut at 0.
    CHECK (movieExportRange (VideoLength::trimToVideoTake, 12 * 48000, { -480, 48000 }) == SampleRange { 0, 48000 });
}

TEST_CASE ("the full song runs from 0 to the later of the mix's end and the Video Take's end", "[export][mp4][span]")
{
    CHECK (movieExportRange (VideoLength::fullSong, 12 * 48000, { 48000, 11 * 48000 }) == SampleRange { 0, 12 * 48000 });
    CHECK (movieExportRange (VideoLength::fullSong, 2 * 48000, { 48000, 5 * 48000 }) == SampleRange { 0, 5 * 48000 });
    CHECK (movieExportRange (VideoLength::fullSong, 12 * 48000, { -480, 48000 }) == SampleRange { 0, 12 * 48000 });
}
