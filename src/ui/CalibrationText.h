#pragma once

#include "capture/ClapMeasurement.h"
#include "capture/LoopbackMeasurement.h"
#include "song/Song.h"

#include <optional>
#include <string>

// What the calibration panel (ui/CalibrationPanel.h, issue #39) says about the calibrations in
// use and about a measurement. Pure.
namespace overdub::ui
{
// The stored loopback calibration for the open device and rate (none: never calibrated).
std::string latencyCalibrationText (std::optional<song::SampleCount> storedOffset, int sampleRate);

// A loopback run's measurement: how late Takes landed with the calibration in use, or why it
// cannot be used (no signal, too much jitter).
std::string latencyResultText (const capture::LoopbackMeasurement& measurement, int sampleRate);

// The stored camera calibration for the camera and output device (none: never calibrated).
std::string cameraCalibrationText (std::optional<double> storedOffsetMs);

// A clap measurement: how far the video sat from the audio, or that no clap was seen.
std::string clapResultText (const capture::ClapReport& report);
} // namespace overdub::ui
