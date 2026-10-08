#pragma once

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
} // namespace overdub::ui::colours
