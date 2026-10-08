#pragma once

#include "song/Song.h"

#include <optional>
#include <string>

// The JUCE plugin identifier of an AU effect and back (issue #33). JUCE names an AU
// "AudioUnit:<category>/<type>,<subtype>,<manufacturer>" (AudioUnitPluginFormat); an Insert Chain
// slot stores the three four-character codes (song::AudioUnitComponent).
namespace overdub::plugin
{
// "AudioUnit:Effects/aufx,dely,appl". Every Insert is an effect, so the category is always Effects.
std::string identifierFor (const song::AudioUnitComponent& component);

// The three codes after the last ':' or '/'; none unless there are exactly three of 4 characters.
std::optional<song::AudioUnitComponent> componentFromIdentifier (const std::string& identifier);

// "aufx,dely,appl", for logs.
std::string componentText (const song::AudioUnitComponent& component);
} // namespace overdub::plugin
