// PROTOTYPE — issue #6 spike.
// Plays noise bursts as the Backing Track, records both armed tracks dry, then cross-correlates
// take 0 against the source to measure the real round trip and the error of the compensation.
#pragma once
#include "SpikeEngine.h"

struct LoopbackResult
{
    bool signalFound = false;
    int expected = 0;            // input + output + graph latency, i.e. the compensation applied
    int measuredMedian = 0;      // raw round trip, from the cross-correlation peak
    int errorSamples = 0;        // measuredMedian - expected; > 0 means takes land late
    int spreadSamples = 0;       // max - min across bursts (jitter)
    double peakCorrelation = 0;  // normalised, 0..1; tells whether the burst was actually captured
    juce::Array<int> lags;
    juce::int64 dropped = 0;
    juce::StringPairArray toFields (double sampleRate) const;
};

// Runs on the message thread; calls done once the take has been recorded and analysed.
void runLoopbackTest (SpikeEngine& engine, std::function<void (LoopbackResult)> done);
