#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>

// What hosts a slot's processor, for its editor window's badge (issue #34, ADR 0002), and the AU
// instance a crash notification names (issue #40).
namespace overdub::plugin
{
// "AUv3 · out-of-process" or "AUv2 · out-of-process" for an AU (AudioUnitLoader creates every AU
// out-of-process, ADR 0002); "in-process" for any other processor (the test-only ones).
std::string hostingBadge (juce::AudioProcessor& processor);

// The AU instance (AudioUnit) behind an AU processor, as kAudioComponentInstanceInvalidationNotification
// names it (issue #40); null for any other processor.
const void* audioUnitHandle (const juce::AudioProcessor& processor);
} // namespace overdub::plugin
