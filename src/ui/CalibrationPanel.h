#pragma once

#include "capture/CameraCalibration.h"
#include "capture/ClapSelftest.h"
#include "capture/LoopbackMeasurement.h"
#include "core/Error.h"
#include "song/Song.h"
#include "ui/Intent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <string>
#include <variant>

// The settings window's calibration section (issue #39, ADR 0002, ADR 0003), under the device
// picker. Both calibrations are per machine, never in a Song.
//
// Latency: shows the loopback calibration stored for the open input device, output device and
// sample rate (or that there is none), how to patch the loopback, and Measure. A usable measurement
// (bursts found, jitter within ADR 0005's limit) offers "Use This Calibration", which stores the
// offset in use plus the measured one (a zero result is offered only to a device never calibrated).
//
// Camera: shows the camera calibration stored for the Song's camera and the output device, how to
// record a clap Pass, and Measure Last Clap Pass. A measurement of the open Song's newest Video
// Take with a clap seen offers the calibration it was placed with plus the mean clap offset, for
// that Video Take's camera and the output device its Pass played through (issue #68). A Video Take
// that does not know its output device (format 1) offers nothing and says why.
//
// A view (issue #170): the ui::Workspace owns the CalibrationState, so measuring works with the
// Settings window closed, and the panel shows it. Its buttons run the App Intents
// (measure_latency, save_latency_calibration, measure_clap_pass, save_camera_calibration) through
// onIntent. The texts are ui/CalibrationText.h.
namespace overdub::ui
{
// The calibrations stored for what is open. Without an open device (deviceOpen false) no loopback
// can run; without a camera id no clap Pass can be measured for a camera.
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

// A loopback run is running: Measure is unavailable until it ends.
struct LatencyMeasuring
{
};

// A loopback run's outcome, and the calibration it offers to store (none when unusable).
struct LatencyMeasured
{
    core::Expected<capture::LoopbackMeasurement> outcome;
    std::optional<intent::SaveLatencyCalibration> toStore;
};

// A clap measurement's outcome, and the calibration it offers to store (none when unusable).
struct ClapMeasured
{
    core::Expected<capture::VideoTakeClaps> outcome;
    std::optional<intent::SaveCameraCalibration> toStore;
};

// What a clap measurement's save stored, for the output device its Pass played through, which may
// not be the one open now (issue #88).
struct CameraSaved
{
    intent::SaveCameraCalibration stored;
};

using LatencyRun = std::variant<std::monostate, LatencyMeasuring, LatencyMeasured>;
using ClapRun = std::variant<std::monostate, ClapMeasured, CameraSaved>;

// Everything the calibration section shows. The runs' results stay until a save, another Song or
// another device replaces them.
struct CalibrationState
{
    CalibrationStatus status;
    LatencyRun latency;
    ClapRun clap;
};

// The run's outcome with what it offers: the offset in use plus the measured one, when the
// measurement passed and changes something (a zero result marks a device never calibrated as
// calibrated, so it is offered to that device only).
LatencyMeasured latencyMeasured (core::Expected<capture::LoopbackMeasurement> outcome, std::optional<song::SampleCount> offsetInUse);

// The measurement with what it offers: the calibration the Video Take was placed with plus the
// mean clap offset, for its camera and output device, when a clap was seen and the output device
// is known.
ClapMeasured clapMeasured (core::Expected<capture::VideoTakeClaps> outcome);

class CalibrationPanel final : public juce::Component
{
public:
    CalibrationPanel();

    void show (const CalibrationState& state);

    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

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
    void showLatencyRun (const LatencyRun& run);
    void showClapRun (const ClapRun& run);
    void runIntent (const AppIntent& intent);

    CalibrationState current;
    juce::Label latencyTitle { {}, "Latency calibration" };
    juce::Label cameraTitle { {}, "Camera calibration" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CalibrationPanel)
};
} // namespace overdub::ui
