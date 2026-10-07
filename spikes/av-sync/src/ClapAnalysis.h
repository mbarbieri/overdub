// PROTOTYPE — issue #7 spike.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// Clap test. Finds audio transients (sample-accurate), then writes a contact sheet per
// clap: every video frame within ±300 ms, labelled Δ = frame time − clap time.
// The frame where the hands meet gives the A/V offset (positive = video late), to ±½ frame.
struct ClapAnalysisInput
{
    juce::String label;
    std::vector<float> audio;
    double sampleRate = 0;
    juce::File videoFile;
    double videoStartSample = 0;      // audio sample at which movie time 0 plays
    double movieSecondsPerSample = 0; // 1 / sampleRate, corrected for drift where it applies
    juce::File outputDir;
};

// Writes clap-NN.png and summary.txt into outputDir; returns the summary text.
juce::String analyseClaps (const ClapAnalysisInput& input);

// Decodes a movie's first audio track to mono float at sampleRate. firstPtsSeconds
// is where that audio starts on the movie timeline (AAC priming shows up here if it leaks).
bool readMovieAudio (const juce::File& movie, double sampleRate, std::vector<float>& audio,
                     double& firstPtsSeconds, juce::String& error);
