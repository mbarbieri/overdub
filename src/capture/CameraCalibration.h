#pragma once

#include "core/Error.h"
#include "song/Song.h"

#include <juce_core/juce_core.h>

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

// Stores (or replaces) the offset for this key, keeping every other key. Creates the folder.
// Errors: capture.camera_calibration_unreadable (the existing file),
// capture.camera_calibration_write_failed (path).
core::Expected<void> writeCameraCalibration (const juce::File& file, const CameraCalibrationKey& key, double offsetMs);

// The offset in whole samples at this rate, rounded to the nearest.
song::SampleCount calibrationSamples (double offsetMs, int sampleRate);
} // namespace overdub::capture
