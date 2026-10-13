#include "ui/CalibrationPanel.h"

#include "capture/CameraCalibration.h"
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

    measureLatencyButton.onClick = [this] {
        if (onMeasureLatency)
            onMeasureLatency();
    };
    saveLatencyButton.onClick = [this] {
        if (latencyToStore.has_value() && onSaveLatency)
            onSaveLatency (*latencyToStore);
    };
    measureClapButton.onClick = [this] {
        if (onMeasureClap)
            onMeasureClap();
    };
    saveCameraButton.onClick = [this] {
        if (cameraToStore.has_value() && onSaveCamera)
            onSaveCamera (cameraToStore->first, cameraToStore->second);
    };

    for (auto* child : std::initializer_list<juce::Component*> { &latencyTitle, &deviceLabel, &latencyStatusLabel, &cableInstructions, &measureLatencyButton, &latencyResultLabel, &cameraTitle, &cameraStatusLabel, &clapInstructions, &measureClapButton, &clapResultLabel })
        addAndMakeVisible (child);
    addChildComponent (saveLatencyButton);
    addChildComponent (saveCameraButton);
}

void CalibrationPanel::show (const CalibrationStatus& status)
{
    current = status;
    latencyToStore.reset();
    cameraToStore.reset();

    deviceLabel.setText (status.deviceOpen ? utf8 (status.deviceText) : "No audio device is open.", juce::dontSendNotification);
    latencyStatusLabel.setText (status.deviceOpen ? utf8 (latencyCalibrationText (status.latencyOffset, status.sampleRate)) : "", juce::dontSendNotification);
    measureLatencyButton.setEnabled (status.deviceOpen);
    latencyResultLabel.setText ({}, juce::dontSendNotification);
    saveLatencyButton.setVisible (false);

    const auto camera = status.cameraId.empty() ? std::string ("No camera chosen: pick one in Record mode.")
                                                : (status.cameraName.empty() ? status.cameraId : status.cameraName) + ". " + cameraCalibrationText (status.cameraOffsetMs);
    cameraStatusLabel.setText (utf8 (camera), juce::dontSendNotification);
    measureClapButton.setEnabled (! status.cameraId.empty() && status.deviceOpen);
    clapResultLabel.setText ({}, juce::dontSendNotification);
    saveCameraButton.setVisible (false);
}

void CalibrationPanel::showLatencyMeasuring()
{
    measureLatencyButton.setEnabled (false);
    saveLatencyButton.setVisible (false);
    latencyToStore.reset();
    latencyResultLabel.setText ("Measuring: noise bursts play for about 7 seconds...", juce::dontSendNotification);
}

void CalibrationPanel::showLatencyResult (const core::Expected<capture::LoopbackMeasurement>& result)
{
    measureLatencyButton.setEnabled (current.deviceOpen);
    latencyToStore.reset();
    if (! result.has_value())
    {
        latencyResultLabel.setText (utf8 (sentenceFor (result.error().code)), juce::dontSendNotification);
        saveLatencyButton.setVisible (false);
        return;
    }

    latencyResultLabel.setText (utf8 (latencyResultText (*result, current.sampleRate)), juce::dontSendNotification);
    // Offsets measured with the calibration in use: storing their sum lands Takes where heard. A
    // zero result changes nothing, except on a device never calibrated, where storing it marks the
    // device calibrated (no more ui.latency_not_calibrated).
    if (capture::loopbackPassed (*result) && (result->offsetSamples != 0 || ! current.latencyOffset.has_value()))
        latencyToStore = current.latencyOffset.value_or (0) + result->offsetSamples;
    saveLatencyButton.setVisible (latencyToStore.has_value());
}

void CalibrationPanel::showClapResult (const core::Expected<capture::VideoTakeClaps>& result)
{
    cameraToStore.reset();
    if (! result.has_value())
    {
        clapResultLabel.setText (utf8 (sentenceFor (result.error().code)), juce::dontSendNotification);
        saveCameraButton.setVisible (false);
        return;
    }

    clapResultLabel.setText (utf8 (clapResultText (result->report, result->outputDevice)), juce::dontSendNotification);
    if (result->report.meanOffsetMs.has_value() && ! result->outputDevice.empty())
        cameraToStore = std::pair { capture::CameraCalibrationKey { result->cameraId, result->outputDevice },
                                    capture::calibratedCameraOffsetMs (result->appliedCalibrationSamples, result->sampleRate, *result->report.meanOffsetMs) };
    saveCameraButton.setVisible (cameraToStore.has_value());
}

void CalibrationPanel::showCameraSaved (const capture::CameraCalibrationKey& key, double offsetMs)
{
    cameraToStore.reset();
    saveCameraButton.setVisible (false);
    clapResultLabel.setText (utf8 (cameraCalibrationSavedText (offsetMs, key.outputDevice)), juce::dontSendNotification);
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
