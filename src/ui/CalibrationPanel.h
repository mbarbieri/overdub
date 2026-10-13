#pragma once

#include "capture/CameraCalibration.h"
#include "capture/ClapSelftest.h"
#include "capture/LoopbackMeasurement.h"
#include "core/Error.h"
#include "song/Song.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>

// The settings window's calibration section (issue #39, ADR 0002, ADR 0003), under the device
// picker. Both calibrations are per machine, never in a Song.
//
// Latency: shows the loopback calibration stored for the open input device, output device and
// sample rate (or that there is none), how to patch the loopback, and Measure. The app runs a
// capture::LoopbackRun and hands the measurement back; a usable one (bursts found, jitter within
// ADR 0005's limit) offers "Use This Calibration", which hands back the offset in use plus the
// measured one: the offset to store (a zero result is offered only to a device never calibrated).
//
// Camera: shows the camera calibration stored for the Song's camera and the output device, how to
// record a clap Pass, and Measure Last Clap Pass. The app measures the open Song's newest Video
// Take (capture::measureNewestVideoTake); a result with a clap seen offers the calibration it was
// placed with plus the mean clap offset, for that Video Take's camera and the output device its Pass
// played through (issue #68). A Video Take that does not know its output device (format 1) offers
// nothing and says why.
//
// Thin: the texts are ui/CalibrationText.h; the app does the measuring and the storing.
namespace overdub::ui
{
// What the panel shows. Without an open device (deviceOpen false) no loopback can run; without a
// camera id no clap Pass can be measured for a camera.
struct CalibrationStatus
{
    bool deviceOpen = false;
    std::string deviceText;                         // e.g. "Mic > Speakers, 48000 Hz"
    int sampleRate = 0;                             // the open device's
    std::optional<song::SampleCount> latencyOffset; // stored for this device and rate; none: never
    std::string cameraId;                           // the open Song's camera choice; "" for none
    std::string cameraName;
    std::optional<double> cameraOffsetMs; // stored for this camera and output device; none: never
};

class CalibrationPanel final : public juce::Component
{
public:
    CalibrationPanel();

    // Shows the calibrations in use and clears any result.
    void show (const CalibrationStatus& status);
    // A loopback run started: Measure is disabled until its result arrives.
    void showLatencyMeasuring();
    void showLatencyResult (const core::Expected<capture::LoopbackMeasurement>& result);
    void showClapResult (const core::Expected<capture::VideoTakeClaps>& result);
    // After a clap calibration was stored (and show() refreshed the status): says what was stored
    // and for which output device, which may not be the one open now (issue #88).
    void showCameraSaved (const capture::CameraCalibrationKey& key, double offsetMs);

    void resized() override;
    void paint (juce::Graphics& g) override;

    std::function<void()> onMeasureLatency;
    std::function<void (song::SampleCount offsetToStore)> onSaveLatency;
    std::function<void()> onMeasureClap;
    std::function<void (const capture::CameraCalibrationKey& key, double offsetMsToStore)> onSaveCamera;

    juce::Label deviceLabel;
    juce::Label latencyStatusLabel;
    juce::Label cableInstructions;
    juce::TextButton measureLatencyButton { "Measure Latency" };
    juce::Label latencyResultLabel;
    juce::TextButton saveLatencyButton { "Use This Calibration" };

    juce::Label cameraStatusLabel;
    juce::Label clapInstructions;
    juce::TextButton measureClapButton { "Measure Last Clap Pass" };
    juce::Label clapResultLabel;
    juce::TextButton saveCameraButton { "Use This Calibration" };

private:
    CalibrationStatus current;
    std::optional<song::SampleCount> latencyToStore;
    std::optional<std::pair<capture::CameraCalibrationKey, double>> cameraToStore;
    juce::Label latencyTitle { {}, "Latency calibration" };
    juce::Label cameraTitle { {}, "Camera calibration" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalibrationPanel)
};
} // namespace overdub::ui
