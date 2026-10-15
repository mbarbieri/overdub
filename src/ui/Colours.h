#pragma once

#include "song/Song.h"

#include <juce_graphics/juce_graphics.h>

// The main window's few colours, after the prototype on prototype/main-window (issue #9).
namespace overdub::ui::colours
{
inline const juce::Colour background { 0xff1c1c1e };
inline const juce::Colour panel { 0xff2a2a2d };
inline const juce::Colour line { 0xff3a3a3d };
inline const juce::Colour text { 0xffe8e8ea };
inline const juce::Colour dimText { 0xff8e8e93 };
inline const juce::Colour accent { 0xff4f8cff };
inline const juce::Colour rec { 0xffff453a };
inline const juce::Colour notice { 0xff3a3a1e };
inline const juce::Colour mute { 0xffff9f0a };
inline const juce::Colour solo { 0xffffd60a };
inline const juce::Colour takeClip { 0xff3d5a80 };
inline const juce::Colour backingClip { 0xff4a6b4a };
inline const juce::Colour envelope { 0xffa9b0ff };
inline const juce::Colour videoTake { 0xff8a5a8a };
// Text and lines drawn over a Track's pastel colour (Clip names, waveforms, fade lines).
inline const juce::Colour onTrackColour { 0xff1c1c1e };
} // namespace overdub::ui::colours

// A Track's own colour (issue #137, song/TrackColours.h), on its Edit header and Record row
// stripes, its Clips, its Take Lanes and its Mixer strip.
namespace overdub::ui
{
// The width of the colour stripe along the left edge of an Edit Track header and a Record row.
inline constexpr int trackColourStripeWidth = 4;

// The Track's "#rrggbb" as an opaque colour; colours::dimText when it is not in that form.
juce::Colour trackColour (const song::Track& track);

// The Take Lanes' dimmed colour: each channel of trackColour at 60 %, rounded.
juce::Colour takeLaneColour (const song::Track& track);
} // namespace overdub::ui
