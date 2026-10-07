#pragma once

#include "core/Error.h"
#include "song/Peaks.h"
#include "song/Song.h"

#include <optional>
#include <vector>

// The Edit timeline's geometry and gestures (issue #28, ADR 0005 UI amendment), pure and without
// JUCE: Song sample <-> x, hit-testing a lane's Clips, a drag -> its Clip operation, and the
// waveform columns drawn from the peaks cache. The Timeline Component only calls these.
//
// Zoom is a whole number of samples per pixel (a power of two between the limits), so sample <-> x
// is integer arithmetic: a drag of n pixels is exactly n * samplesPerPixel samples at every zoom.
namespace overdub::ui
{
using song::SampleCount;

inline constexpr SampleCount minSamplesPerPixel = 16;
inline constexpr SampleCount maxSamplesPerPixel = 16384;
inline constexpr SampleCount defaultSamplesPerPixel = 512;
// The grab zone, in pixels, at each end of a Clip that trims instead of moving.
inline constexpr int clipEdgeGrabWidth = 6;

// Which part of the Song the lanes show: firstSample sits at x = laneX.
struct TimelineView
{
    SampleCount firstSample = 0;
    SampleCount samplesPerPixel = defaultSamplesPerPixel;
    int laneX = 0;

    bool operator== (const TimelineView&) const = default;
};

// The pixel column holding this sample (rounding down, also left of laneX).
int xForSample (const TimelineView& view, SampleCount sample);
// The first sample of this pixel column.
SampleCount sampleForX (const TimelineView& view, int x);

// Half or twice the samples per pixel, kept within the limits; firstSample is kept.
TimelineView zoomedIn (TimelineView view);
TimelineView zoomedOut (TimelineView view);

// A Clip's column range [left, right) in a lane: at least 2 pixels wide so it stays visible.
struct ClipColumns
{
    int left = 0;
    int right = 0;
};

ClipColumns clipColumns (const TimelineView& view, const song::Clip& clip);

enum class ClipPart
{
    body,
    startEdge,
    endEdge
};

struct ClipHit
{
    song::Id clipId;
    ClipPart part = ClipPart::body;

    bool operator== (const ClipHit&) const = default;
};

// The Clip under column x in a lane, the top layer (last) first. Within clipEdgeGrabWidth of an end
// (narrowed to a third of the Clip's width for narrow Clips) it is that edge, otherwise the body.
std::optional<ClipHit> hitTestClips (const std::vector<song::Clip>& clipsInLayerOrder, const TimelineView& view, int x);

// A drag that started on a Clip part at column downX.
struct ClipDrag
{
    song::Id clipId;
    ClipPart part = ClipPart::body;
    int downX = 0;
};

// The Song the drag makes when the pointer is at column x, from the Song as it was at mouse down:
// body -> moveClip, start edge -> trimClipStart, end edge -> trimClipEnd, each by
// (x - downX) * samplesPerPixel samples and clamped as those operations clamp (song/ClipEdits.h).
// Errors: song.clip_not_found.
core::Expected<song::Song> songForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x);

// The undo step label of a drag on this part: "Move Clip" or "Trim Clip".
const char* labelForDrag (ClipPart part);

// One waveform column per pixel: the min and max over every channel of the peak blocks holding
// source samples [sourceStart + i * samplesPerPixel, + samplesPerPixel). Columns past the peaks
// (or before the source) are {0, 0}.
std::vector<song::PeakRange> waveformColumns (const song::Peaks& peaks, SampleCount sourceStart, SampleCount samplesPerPixel, int numColumns);
} // namespace overdub::ui
