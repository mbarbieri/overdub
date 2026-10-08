#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>

// The per-machine camera calibration (ADR 0003): one offset in milliseconds per camera and output
// device, subtracted when a Video Take is placed (positive: the camera stamps frames late). It also
// absorbs any error in the output device's reported latency, hence the output device in the key.
// Never part of a Song; a Video Take keeps the value applied, in samples. Stored as JSON in
// ~/Library/Application Support/Overdub/camera-calibration.json:
//   { "version": 1, "offsets": [ { "camera_id", "output_device", "offset_ms" } ] }
// A camera with no stored offset uses 0 until the calibration flow (slice 18) measures one.
namespace overdub::capture
{
struct CameraCalibrationKey
{
    std::string cameraId;     // AVCaptureDevice uniqueID
    std::string outputDevice; // audio output device name as JUCE lists it

    bool operator== (const CameraCalibrationKey&) const = default;
};

// ~/Library/Application Support/Overdub/camera-calibration.json
juce::File defaultCameraCalibrationFile();

// The stored offset in ms; 0 when the file or the key does not exist.
// Errors: capture.camera_calibration_unreadable (path, reason).
core::Expected<double> readCameraCalibration (const juce::File& file, const CameraCalibrationKey& key);

// The stored offset in ms; none when the file or the key does not exist (never calibrated).
// Errors: capture.camera_calibration_unreadable (path, reason).
core::Expected<std::optional<double>> findCameraCalibration (const juce::File& file, const CameraCalibrationKey& key);

// Stores (or replaces) the offset for this key, keeping every other key. Creates the folder.
// Errors: capture.camera_calibration_unreadable (the existing file),
// capture.camera_calibration_write_failed (path).
core::Expected<void> writeCameraCalibration (const juce::File& file, const CameraCalibrationKey& key, double offsetMs);

// The calibration to store after a clap measurement (issue #39): the calibration the measured
// Video Take was placed with (in samples at the Song's rate) plus the clap test's mean offset
// (positive: the video was still late), in ms.
double calibratedCameraOffsetMs (song::SampleCount appliedSamples, int sampleRate, double meanOffsetMs);

// The offset in whole samples at this rate, rounded to the nearest.
song::SampleCount calibrationSamples (double offsetMs, int sampleRate);
} // namespace overdub::capture
