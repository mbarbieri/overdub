#pragma once

#include "core/Error.h"

#include <juce_core/juce_core.h>

#include <vector>

// Reads a movie's video frames back for measurement (the clap selftest, and slice 18's camera
// calibration): each frame's time and how much the picture changed since the previous frame.
namespace overdub::capture
{
struct MovieFrame
{
    double seconds = 0.0; // movie time: the frame's PTS minus the first frame's
    double motion = 0.0;  // mean |luma difference| against the previous frame, 0..255 (0 for the first)
};

// Every frame of the movie's first video track, in order.
// Errors: capture.movie_unreadable (path, reason).
core::Expected<std::vector<MovieFrame>> readMovieFrames (const juce::File& movie);
} // namespace overdub::capture
