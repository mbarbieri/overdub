// PROTOTYPE — issue #7 spike.
#pragma once
#include <juce_core/juce_core.h>
#include <functional>

// Structured key=value lines to stderr, a log file, and (async) the UI.
// Never call from the audio thread.
namespace spikelog
{
void install();
void line (const juce::String& event, const juce::StringPairArray& fields);
void line (const juce::String& event, std::initializer_list<std::pair<juce::String, juce::String>> fields = {});
void setUiSink (std::function<void (const juce::String&)> sink);
juce::File logDirectory();
}
