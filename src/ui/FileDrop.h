#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Importing by dropping files on the Edit page's Timeline (issue #123): which dropped items are
// audio files to import. The Import dialog offers the same extensions.
namespace overdub::ui
{
// The Import dialog's file filter: "*.wav;*.aif;...".
juce::String importableAudioPattern();

// The dropped paths that are files with an importable audio extension (any case), in drop order.
// Folders and other files are left out.
std::vector<juce::File> importableAudioFiles (const juce::StringArray& paths);
} // namespace overdub::ui
