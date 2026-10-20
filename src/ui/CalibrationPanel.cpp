#include "ui/CalibrationPanel.h"

#include "capture/CameraCalibration.h"
#include "core/Described.h"
#include "ui/CalibrationText.h"
#include "ui/Colours.h"
#include "ui/UserMessages.h"

namespace overdub::ui
{
namespace
{
constexpr int margin = 16;
constexpr int titleHeight = 24;
constexpr int lineHeight = 20;
constexpr int buttonHeight = 28;
constexpr int buttonWidth = 180;

juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str());
}

void styleLabel (juce::Label& label, juce::Colour colour)
{
    label.setColour (juce::Label::textColourId, colour);
    label.setJustificationType (juce::Justification::topLeft);
    label.setMinimumHorizontalScale (1.0f);
}
} // namespace

LatencyMeasured latencyMeasured (core::Expected<capture::LoopbackMeasurement> outcome, std::optional<song::SampleCount> offsetInUse)
{
    LatencyMeasured measured { std::move (outcome), std::nullopt };
    // Offsets measured with the calibration in use: storing their sum lands Takes where heard.
    if (const auto& result = measured.outcome; result.has_value() && capture::loopbackPassed (*result) && (result->offsetSamples != 0 || ! offsetInUse.has_value()))
        measured.toStore = intent::SaveLatencyCalibration { offsetInUse.value_or (0) + result->offsetSamples };
    return measured;
}

ClapMeasured clapMeasured (core::Expected<capture::VideoTakeClaps> outcome)
{
    ClapMeasured measured { std::move (outcome), std::nullopt };
    if (const auto& claps = measured.outcome; claps.has_value() && claps->report.meanOffsetMs.has_value() && ! claps->outputDevice.empty())
        measured.toStore = intent::SaveCameraCalibration {
            .cameraId = claps->cameraId,
            .outputDevice = claps->outputDevice,
            .offsetMs = capture::calibratedCameraOffsetMs (claps->appliedCalibrationSamples, claps->sampleRate, *claps->report.meanOffsetMs),
        };
    return measured;
}

CalibrationPanel::CalibrationPanel()
{
    cableInstructions.setText ("Connect a cable from output 1 to input 1 (or turn the speakers up near the microphone) "
                               "and turn off the interface's direct monitoring. Measuring plays short noise bursts.",
                               juce::dontSendNotification);
    clapInstructions.setText ("Arm the camera and record a Pass while clapping 3 to 5 times in frame, "
                              "about a second apart. Then measure it here.",
                              juce::dontSendNotification);

    for (auto* title : { &latencyTitle, &cameraTitle })
    {
        styleLabel (*title, colours::text);
        title->setFont (juce::FontOptions (16.0f, juce::Font::bold));
    }
    for (auto* label : { &deviceLabel, &latencyStatusLabel, &latencyResultLabel, &cameraStatusLabel, &clapResultLabel })
        styleLabel (*label, colours::text);
    for (auto* label : { &cableInstructions, &clapInstructions })
        styleLabel (*label, colours::dimText);

    measureLatencyButton.onClick = [this] { runIntent (intent::MeasureLatency {}); };
    saveLatencyButton.onClick = [this] {
        if (const auto* measured = std::get_if<LatencyMeasured> (&current.latency); measured != nullptr && measured->toStore.has_value())
            runIntent (*measured->toStore);
    };
    measureClapButton.onClick = [this] { runIntent (intent::MeasureClapPass {}); };
    saveCameraButton.onClick = [this] {
        if (const auto* measured = std::get_if<ClapMeasured> (&current.clap); measured != nullptr && measured->toStore.has_value())
            runIntent (*measured->toStore);
    };

    for (auto* child : std::initializer_list<juce::Component*> { &latencyTitle, &deviceLabel, &latencyStatusLabel, &cableInstructions, &measureLatencyButton, &latencyResultLabel, &cameraTitle, &cameraStatusLabel, &clapInstructions, &measureClapButton, &clapResultLabel })
        addAndMakeVisible (child);
    addChildComponent (saveLatencyButton);
    addChildComponent (saveCameraButton);
}

void CalibrationPanel::runIntent (const AppIntent& intent)
{
    if (onIntent)
        onIntent (intent);
}

void CalibrationPanel::show (const CalibrationState& state)
{
    current = state;
    const auto& status = state.status;
    deviceLabel.setText (status.deviceOpen ? utf8 (status.deviceText) : "No audio device is open.", juce::dontSendNotification);
    latencyStatusLabel.setText (status.deviceOpen ? utf8 (latencyCalibrationText (status.latencyOffset, status.sampleRate)) : "", juce::dontSendNotification);
    const auto camera = status.cameraId.empty() ? std::string ("No camera chosen: pick one in Record mode.")
                                                : (status.cameraName.empty() ? status.cameraId : status.cameraName) + ". " + cameraCalibrationText (status.cameraOffsetMs);
    cameraStatusLabel.setText (utf8 (camera), juce::dontSendNotification);
    measureClapButton.setEnabled (! status.cameraId.empty() && status.deviceOpen);
    showLatencyRun (state.latency);
    showClapRun (state.clap);
}

void CalibrationPanel::showLatencyRun (const LatencyRun& run)
{
    measureLatencyButton.setEnabled (current.status.deviceOpen && ! std::holds_alternative<LatencyMeasuring> (run));
    const auto [text, offered] = core::visitExhaustively (
        run,
        core::Overloaded {
            [] (const std::monostate&) { return std::pair { std::string(), false }; },
            [] (const LatencyMeasuring&) { return std::pair { std::string ("Measuring: noise bursts play for about 7 seconds..."), false }; },
            [this] (const LatencyMeasured& measured) {
        const auto& outcome = measured.outcome;
        return std::pair { outcome.has_value() ? latencyResultText (*outcome, current.status.sampleRate) : sentenceFor (outcome.error().code), measured.toStore.has_value() };
    },
        });
    latencyResultLabel.setText (utf8 (text), juce::dontSendNotification);
    saveLatencyButton.setVisible (offered);
}

void CalibrationPanel::showClapRun (const ClapRun& run)
{
    const auto [text, offered] = core::visitExhaustively (
        run,
        core::Overloaded {
            [] (const std::monostate&) { return std::pair { std::string(), false }; },
            [] (const ClapMeasured& measured) {
        const auto& outcome = measured.outcome;
        return std::pair { outcome.has_value() ? clapResultText (outcome->report, outcome->outputDevice) : sentenceFor (outcome.error().code), measured.toStore.has_value() };
    },
            [] (const CameraSaved& saved) { return std::pair { cameraCalibrationSavedText (saved.stored.offsetMs, saved.stored.outputDevice), false }; },
        });
    clapResultLabel.setText (utf8 (text), juce::dontSendNotification);
    saveCameraButton.setVisible (offered);
}

void CalibrationPanel::resized()
{
    auto area = getLocalBounds().reduced (margin);
    const auto row = [&area] (juce::Component& component, int height) {
        component.setBounds (area.removeFromTop (height));
        area.removeFromTop (4);
    };
    const auto buttons = [&area] (juce::Component& left, juce::Component& right) {
        auto line = area.removeFromTop (buttonHeight);
        left.setBounds (line.removeFromLeft (buttonWidth));
        line.removeFromLeft (8);
        right.setBounds (line.removeFromLeft (buttonWidth));
        area.removeFromTop (4);
    };

    row (latencyTitle, titleHeight);
    row (deviceLabel, lineHeight);
    row (latencyStatusLabel, lineHeight * 2);
    row (cableInstructions, lineHeight * 3);
    buttons (measureLatencyButton, saveLatencyButton);
    row (latencyResultLabel, lineHeight * 2);
    area.removeFromTop (12);
    row (cameraTitle, titleHeight);
    row (cameraStatusLabel, lineHeight * 2);
    row (clapInstructions, lineHeight * 3);
    buttons (measureClapButton, saveCameraButton);
    row (clapResultLabel, lineHeight * 2);
}

void CalibrationPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::line);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
}
} // namespace overdub::ui
