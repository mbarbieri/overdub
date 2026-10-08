#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>

// What hosts a slot's processor, for its editor window's badge (issue #34, ADR 0002).
namespace overdub::plugin
{
// "AUv3 · out-of-process" or "AUv2 · out-of-process" for an AU (AudioUnitLoader creates every AU
// out-of-process, ADR 0002); "in-process" for any other processor (the test-only ones).
std::string hostingBadge (juce::AudioProcessor& processor);
} // namespace overdub::plugin
