// PROTOTYPE — issue #6 spike. Scripted measurement run, driven by command-line flags:
//   --auto --plugin1 NAME --plugin2 NAME [--oop] [--buffers 64,128,256] [--cpu-seconds 5] [--editors] [--quit]
// Writes one RESULT line per buffer size to the log and to results-<stamp>.txt.
#pragma once
#include "SpikeEngine.h"
#include "PluginWindow.h"

class AutoRun
{
public:
    AutoRun (SpikeEngine& engine, const juce::StringArray& args);
    void start();

private:
    using Step = std::function<void (std::function<void()> next)>;
    void runNext();
    void addPluginSteps();
    void addBufferSteps (int bufferSize);
    void addEditorSteps();
    void writeResult (const juce::String& line);
    juce::String argValue (const juce::String& flag, const juce::String& fallback) const;

    SpikeEngine& engine;
    juce::StringArray args;
    std::vector<Step> steps;
    size_t nextStep = 0;
    juce::File resultsFile;
    juce::OwnedArray<PluginWindow> windows;
};
