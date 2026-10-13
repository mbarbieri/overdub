#include "ui/CalibrationPanel.h"

#include "ui/UiHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

using overdub::capture::CameraCalibrationKey;
using overdub::capture::ClapReport;
using overdub::capture::ClapResult;
using overdub::capture::LoopbackMeasurement;
using overdub::capture::VideoTakeClaps;
using overdub::song::SampleCount;
using overdub::ui::CalibrationPanel;
using overdub::ui::CalibrationStatus;
using namespace overdub::ui::testing;

namespace
{
CalibrationStatus builtInDevices()
{
    return CalibrationStatus {
        .deviceOpen = true,
        .deviceText = "MacBook Pro Microphone > MacBook Pro Speakers, 48000 Hz",
        .sampleRate = 48000,
        .latencyOffset = std::nullopt,
        .cameraId = "camera-facetime",
        .cameraName = "FaceTime HD Camera",
        .cameraOffsetMs = std::nullopt,
    };
}

LoopbackMeasurement measured (SampleCount offset, SampleCount jitter = 1)
{
    LoopbackMeasurement measurement;
    measurement.signalFound = true;
    measurement.peakCorrelation = 0.9;
    measurement.offsetSamples = offset;
    measurement.jitterSamples = jitter;
    return measurement;
}

VideoTakeClaps clapsWithMeanOffset (double meanOffsetMs, SampleCount appliedSamples)
{
    VideoTakeClaps claps;
    claps.report.claps = { ClapResult { .offsetMs = meanOffsetMs } };
    claps.report.meanOffsetMs = meanOffsetMs;
    claps.report.pass = true;
    claps.cameraId = "camera-facetime";
    claps.outputDevice = "Scarlett 2i2 USB";
    claps.appliedCalibrationSamples = appliedSamples;
    claps.sampleRate = 48000;
    return claps;
}

std::string textOf (const juce::Label& label)
{
    return label.getText().toStdString();
}
} // namespace

TEST_CASE ("the calibration panel shows the device and camera calibrations in use", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);

    panel.show (builtInDevices());
    CHECK (textOf (panel.deviceLabel) == "MacBook Pro Microphone > MacBook Pro Speakers, 48000 Hz");
    CHECK (textOf (panel.latencyStatusLabel) == "Not calibrated: Takes are placed by the latency the device reports.");
    CHECK (textOf (panel.cameraStatusLabel) == "FaceTime HD Camera. Not calibrated: 0 ms.");
    CHECK (isShown (panel.cableInstructions));
    CHECK (panel.measureLatencyButton.isEnabled());
    CHECK_FALSE (isShown (panel.saveLatencyButton));

    auto calibrated = builtInDevices();
    calibrated.latencyOffset = -821;
    calibrated.cameraOffsetMs = 35.0;
    panel.show (calibrated);
    CHECK (textOf (panel.latencyStatusLabel) == "Calibrated: -821 samples (-17.1 ms).");
    CHECK (textOf (panel.cameraStatusLabel) == "FaceTime HD Camera. Calibrated: 35.0 ms (video late).");
}

TEST_CASE ("without a device or a camera, the panel says so and offers no measurement", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);

    panel.show (CalibrationStatus {});
    CHECK (textOf (panel.deviceLabel) == "No audio device is open.");
    CHECK_FALSE (panel.measureLatencyButton.isEnabled());
    CHECK (textOf (panel.cameraStatusLabel) == "No camera chosen: pick one in Record mode.");
    CHECK_FALSE (panel.measureClapButton.isEnabled());
}

TEST_CASE ("Measure starts a loopback run; a usable result offers its calibration, added to the one in use", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);
    auto status = builtInDevices();
    status.latencyOffset = -800;
    panel.show (status);

    int measureRequests = 0;
    std::optional<SampleCount> saved;
    panel.onMeasureLatency = [&] { ++measureRequests; };
    panel.onSaveLatency = [&] (SampleCount offset) { saved = offset; };

    click (panel.measureLatencyButton);
    CHECK (measureRequests == 1);

    panel.showLatencyMeasuring();
    CHECK_FALSE (panel.measureLatencyButton.isEnabled());
    CHECK (textOf (panel.latencyResultLabel) == "Measuring: noise bursts play for about 7 seconds...");

    panel.showLatencyResult (measured (-21));
    CHECK (panel.measureLatencyButton.isEnabled());
    CHECK (textOf (panel.latencyResultLabel) == "Takes landed 21 samples (0.4 ms) early, jitter 1 samples. Use this calibration to correct it.");
    REQUIRE (isShown (panel.saveLatencyButton));

    click (panel.saveLatencyButton);
    CHECK (saved == SampleCount { -821 });
}

TEST_CASE ("a loopback result that cannot be used, or a failed run, offers no calibration", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);
    panel.show (builtInDevices());

    panel.showLatencyResult (measured (200, 12));
    CHECK_FALSE (isShown (panel.saveLatencyButton));
    CHECK (textOf (panel.latencyResultLabel).starts_with ("The bursts came back unevenly"));

    panel.showLatencyResult (LoopbackMeasurement {});
    CHECK_FALSE (isShown (panel.saveLatencyButton));

    panel.showLatencyResult (std::unexpected (overdub::core::Error { "capture.calibration_timeout", "the audio device stopped", {} }));
    CHECK_FALSE (isShown (panel.saveLatencyButton));
    CHECK (textOf (panel.latencyResultLabel) == "The audio device stopped during the measurement. Check the device, then measure again.");
    CHECK (panel.measureLatencyButton.isEnabled());
}

TEST_CASE ("measuring the last clap Pass offers the camera calibration it implies", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);
    panel.show (builtInDevices());

    int measureRequests = 0;
    std::optional<std::pair<CameraCalibrationKey, double>> saved;
    panel.onMeasureClap = [&] { ++measureRequests; };
    panel.onSaveCamera = [&] (const CameraCalibrationKey& key, double offsetMs) { saved = { key, offsetMs }; };

    click (panel.measureClapButton);
    CHECK (measureRequests == 1);

    // Placed with 480 samples (10 ms) and still 25 ms late: store 35 ms for that camera and the
    // output device the clap Pass played through, not the one open now (issue #68).
    panel.showClapResult (clapsWithMeanOffset (25.0, 480));
    CHECK (textOf (panel.clapResultLabel) == "1 of 1 claps seen: the video was 25.0 ms late on Scarlett 2i2 USB. Use this calibration to correct it.");
    REQUIRE (isShown (panel.saveCameraButton));
    click (panel.saveCameraButton);
    REQUIRE (saved.has_value());
    CHECK (saved->first == CameraCalibrationKey { "camera-facetime", "Scarlett 2i2 USB" });
    CHECK (saved->second == 35.0);

    panel.showClapResult (std::unexpected (overdub::core::Error { "capture.clap_no_video_take", "none", {} }));
    CHECK_FALSE (isShown (panel.saveCameraButton));
    CHECK (textOf (panel.clapResultLabel) == "No Pass has a Video Take yet. Arm the camera and record a Pass clapping in frame.");
}

TEST_CASE ("a clap Pass whose output device is not known offers no camera calibration and says why", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);
    panel.show (builtInDevices());
    int saves = 0;
    panel.onSaveCamera = [&] (const CameraCalibrationKey&, double) { ++saves; };

    auto fromFormat1Song = clapsWithMeanOffset (25.0, 480);
    fromFormat1Song.outputDevice = "";
    panel.showClapResult (fromFormat1Song);

    CHECK_FALSE (isShown (panel.saveCameraButton));
    CHECK (textOf (panel.clapResultLabel).ends_with ("so no calibration can be stored. Record a new clap Pass, then measure again."));
    click (panel.saveCameraButton);
    CHECK (saves == 0);
}

TEST_CASE ("a zero result is offered to a device never calibrated, so it counts as calibrated, and to no other", "[ui][calibration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);
    std::optional<SampleCount> saved;
    panel.onSaveLatency = [&] (SampleCount offset) { saved = offset; };

    panel.show (builtInDevices());
    panel.showLatencyResult (measured (0));
    REQUIRE (isShown (panel.saveLatencyButton));
    click (panel.saveLatencyButton);
    CHECK (saved == SampleCount { 0 });

    auto calibrated = builtInDevices();
    calibrated.latencyOffset = -821;
    panel.show (calibrated);
    panel.showLatencyResult (measured (0));
    CHECK_FALSE (isShown (panel.saveLatencyButton));
}

// Renders the calibration panel for a session to look at (ADR 0005 UI amendment):
// build/ui-snapshots/slice-18/*.png. Nothing here asserts on pixels.
TEST_CASE ("snapshots of the calibration panel, not calibrated and with results to use", "[ui][snapshot][calibration]")
{
    const std::string slice = "slice-18";
    juce::ScopedJuceInitialiser_GUI gui;
    CalibrationPanel panel;
    layOut (panel, 460, 520);

    panel.show (builtInDevices());
    CHECK (writeSnapshot (panel, slice, "calibration-not-calibrated").existsAsFile());

    panel.showLatencyMeasuring();
    CHECK (writeSnapshot (panel, slice, "calibration-measuring").existsAsFile());

    panel.showLatencyResult (measured (-821, 3));
    panel.showClapResult (clapsWithMeanOffset (25.0, 0));
    CHECK (writeSnapshot (panel, slice, "calibration-results").existsAsFile());
}
