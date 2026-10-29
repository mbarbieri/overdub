#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/PeaksLookup.h"
#include "ui/TimelineGeometry.h"

#include <vector>

// Transcribe mode's geometry (issue #225, ADR 0010), pure and without Components: the zoom limits
// of its tall waveform, clamping its view, the wheel and command-wheel, the overview's click, and
// the heard mix's waveform columns. ui::TranscribePage only calls these.
//
// The view is a ui::TimelineView (whole samples per pixel, firstSample at laneX). Zooming in stops
// at about 2 s across the width; zooming out stops once the whole Song (overviewLength) fits.
namespace overdub::ui
{
// About this much of the Song fills the waveform's width at the finest zoom.
inline constexpr double finestTranscribeSeconds = 2.0;
// A command-wheel movement of one unit (JUCE's MouseWheelDetails) zooms by this many levels.
inline constexpr double transcribeWheelZoomLevelsPerUnit = 8.0;

// The power of two nearest (in levels) to the samples per pixel that put finestTranscribeSeconds
// across `width`; at least 1.
SampleCount transcribeMinSamplesPerPixel (int sampleRate, int width);
// The smallest power of two that fits `songLength` in `width`, and never below the minimum.
SampleCount transcribeMaxSamplesPerPixel (SampleCount songLength, int width, SampleCount minimum);

struct TranscribeZoomLimits
{
    SampleCount minimum = 1;
    SampleCount maximum = 1;
};

// Both limits for a Song `songLength` long at `sampleRate`, across `width`.
TranscribeZoomLimits transcribeZoomLimits (int sampleRate, SampleCount songLength, int width);

// samplesPerPixel within [minimum, maximum]; firstSample within [0, songLength - the visible span]
// (0 when the Song is shorter than the span).
TimelineView clampedTranscribeView (TimelineView view, SampleCount minimum, SampleCount maximum, SampleCount songLength, int width);

// How far a wheel or trackpad movement scrolls time, in pixels (positive: later): the horizontal
// movement, or the vertical one when there is none, as shift-wheel scrolls Edit's time.
int transcribeWheelPixels (float deltaX, float deltaY);

// The pinch scale factor a command-wheel movement stands for: up (positive deltaY) zooms in.
float transcribeWheelZoomScale (float deltaY);

// The view's firstSample after a click or drag at x on the overview (`width` wide, spanning
// `songLength`): the visible span centred on the Song sample there, unclamped at the end.
SampleCount overviewViewStart (int x, int width, SampleCount songLength, SampleCount visibleSamples);

// A mix's waveform, one column per pixel from firstSample: per Track that sounds (the Song's mute
// and solo as given, engine::isAudible), the top Clip at each column's first sample adds its
// source's peaks (ui::waveformColumns) times its gain, its Track's volume and the Master's. The
// sum is clamped to [-1, 1]. Fades and pan are not drawn. Columns nothing sounds in are {0, 0}.
std::vector<song::PeakRange> heardMixColumns (const song::Song& song, const PeaksLookup& peaksOf, SampleCount firstSample, SampleCount samplesPerPixel, int numColumns);

// What Transcribe mode's waveform and overview draw (issue #226): the Focus Track alone, whatever
// the Transcribe Mix says of it, or with no Focus Track the heard Transcribe Mix
// (heardMixColumns of song::withTranscribeMix).
std::vector<song::PeakRange> transcribeColumns (const song::Song& song, const PeaksLookup& peaksOf, SampleCount firstSample, SampleCount samplesPerPixel, int numColumns);
} // namespace overdub::ui
