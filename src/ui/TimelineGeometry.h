#pragma once

#include "core/Error.h"
#include "song/Peaks.h"
#include "song/Song.h"

#include <optional>
#include <string>
#include <vector>

// The Edit timeline's geometry and gestures (issues #28, #29, ADR 0005 UI amendment), pure and
// without JUCE: Song sample <-> x, hit-testing a lane's Clips and their fade handles and gain tag,
// a drag -> its Clip operation, and the waveform columns drawn from the peaks cache. The Timeline
// Component only calls these.
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
// A fade handle: a square in a Clip's top row, centred on where its fade ends.
inline constexpr int fadeHandleSize = 8;
// The dB tag in a Clip's top-right corner, just under the fade handles' row (so a handle never
// covers it) and left of the end edge's grab zone.
inline constexpr int gainTagWidth = 44;
inline constexpr int gainTagHeight = 12;
// Dragging the dB tag up by one pixel raises the gain by this much (rounded to 0.1 dB).
inline constexpr double gainDbPerPixel = 0.1;

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

// What a mouse wheel or trackpad movement scrolls (issue #127), in pixels: a vertical movement
// scrolls the Track rows (positive: down the list), a horizontal one scrolls time (positive:
// later); with shift a vertical wheel scrolls time instead. Each axis is independent, so a
// diagonal swipe scrolls both. Deltas are JUCE's MouseWheelDetails (about 400 pixels per unit).
struct WheelScroll
{
    int timePixels = 0;
    int verticalPixels = 0;
};

WheelScroll wheelScroll (float deltaX, float deltaY, bool shift);

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
    endEdge,
    fadeInHandle,
    fadeOutHandle,
    gainTag
};

// A pixel rectangle [x, x + width) x [y, y + height).
struct PixelRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool isEmpty() const { return width <= 0 || height <= 0; }
    bool contains (int px, int py) const { return px >= x && px < x + width && py >= y && py < y + height; }
    bool operator== (const PixelRect&) const = default;
};

// Where a Clip's handles are drawn and grabbed, for a Clip whose rectangle's top is at clipTop.
// Each fade handle is centred on its fade's inner end, kept inside the Clip's columns. The gain tag
// is empty when the Clip is narrower than gainTagWidth + 2 * clipEdgeGrabWidth + 4 pixels.
struct ClipHandleBounds
{
    PixelRect fadeIn;
    PixelRect fadeOut;
    PixelRect gainTag;
};

ClipHandleBounds clipHandleBounds (const TimelineView& view, const song::Clip& clip, int clipTop);

// The handle of this Clip under (x, y): the fade-in handle, then the fade-out handle, then the gain
// tag; none when the point is on none of them.
std::optional<ClipPart> hitTestClipHandles (const TimelineView& view, const song::Clip& clip, int clipTop, int x, int y);

struct ClipHit
{
    song::Id clipId;
    ClipPart part = ClipPart::body;

    bool operator== (const ClipHit&) const = default;
};

// The Clip under column x in a lane, the top layer (last) first. Within clipEdgeGrabWidth of an end
// (narrowed to a third of the Clip's width for narrow Clips) it is that edge, otherwise the body.
std::optional<ClipHit> hitTestClips (const std::vector<song::Clip>& clipsInLayerOrder, const TimelineView& view, int x);

// A drag that started on a Clip part at (downX, downY).
struct ClipDrag
{
    song::Id clipId;
    ClipPart part = ClipPart::body;
    int downX = 0;
    int downY = 0;
};

// The Song the drag makes when the pointer is at (x, y), from the Song as it was at mouse down,
// clamped as the song/ClipEdits.h operations clamp. With dx = (x - downX) * samplesPerPixel:
// body -> moveClip by dx, start edge -> trimClipStart by dx, end edge -> trimClipEnd by dx,
// fade-in handle -> setClipFadeIn (fade-in + dx), fade-out handle -> setClipFadeOut
// (fade-out - dx), gain tag -> setClipGain (gain - (y - downY) * gainDbPerPixel, rounded to
// 0.1 dB). Only the gain tag reads y. Errors: song.clip_not_found.
core::Expected<song::Song> songForDrag (const song::Song& songAtDown, const ClipDrag& drag, const TimelineView& view, int x, int y);

// "+2.5 dB", "0.0 dB", "-12.0 dB": a Clip gain rounded to 0.1 dB, signed unless zero.
std::string decibelsText (double gainDb);

// The undo step label of a drag on this part: "Move Clip", "Trim Clip", "Fade In", "Fade Out" or
// "Clip Gain".
const char* labelForDrag (ClipPart part);

// One waveform column per pixel: the min and max over every channel of the peak blocks holding
// source samples [sourceStart + i * samplesPerPixel, + samplesPerPixel). Columns past the peaks
// (or before the source) are {0, 0}.
std::vector<song::PeakRange> waveformColumns (const song::Peaks& peaks, SampleCount sourceStart, SampleCount samplesPerPixel, int numColumns);
} // namespace overdub::ui
