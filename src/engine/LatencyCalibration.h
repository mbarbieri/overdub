#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <string>

// The per-machine loopback calibration offsets (ADR 0002): one offset in samples per input
// device, output device and sample rate, added to the reported latency when a Take is placed.
// Never part of a Song. Stored as JSON in ~/Library/Application Support/Overdub/latency-calibration.json:
//   { "version": 1, "offsets": [ { "input_device", "output_device", "sample_rate", "offset_samples" } ] }
// A device with no stored offset uses 0, so reported latency is used until the user calibrates.
namespace overdub::engine
{
struct CalibrationKey
{
    std::string inputDevice;  // device name as JUCE lists it
    std::string outputDevice; // device name as JUCE lists it
    int sampleRate = 0;       // Hz

    bool operator== (const CalibrationKey&) const = default;
};

// ~/Library/Application Support/Overdub/latency-calibration.json
juce::File defaultLatencyCalibrationFile();

// The stored offset for this key; 0 when the file or the key does not exist.
// Errors: engine.calibration_unreadable (path, reason).
core::Expected<song::SampleCount> readCalibrationOffset (const juce::File& file, const CalibrationKey& key);

// Stores (or replaces) the offset for this key, keeping every other key. Creates the folder.
// Errors: engine.calibration_unreadable (the existing file), engine.calibration_write_failed (path).
core::Expected<void> writeCalibrationOffset (const juce::File& file, const CalibrationKey& key, song::SampleCount offsetSamples);
} // namespace overdub::engine
