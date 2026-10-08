#pragma once

#include "export/ExportFormat.h"
#include "song/Song.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>

// Export mode's page (issue #37, after the prototype's Export form): the format (WAV, M4A; MP4
// shown but disabled until slice 17), the Song span, where the file goes, the Export button,
// progress, and Reveal in Finder once a file is written. It stays thin: the request it builds is
// handed to onExport, and what it shows comes from show().
//
// The destination keeps its folder and name when the format changes; only its extension follows.
namespace overdub::ui
{
struct ExportPageState
{
    song::SampleCount clipsEnd = 0; // the end of the Song's last Clip
    int sampleRate = 48000;
    bool waitingForAudio = false; // an export was asked for and waits for the audio to let go
    bool rendering = false;
    double progress = 0.0; // 0..1 while rendering
    std::optional<juce::File> finishedFile;
};

// "Whole mix: 0:00.0 – 1:23.4, plus effect tails".
std::string exportSpanText (song::SampleCount clipsEnd, int sampleRate);
// "Preparing…", "Exporting… 42%", "Exported to /path/Song.wav", or empty when idle.
std::string exportStatusText (const ExportPageState& state);

// A thin bar filled to the fraction done (drawn at once, unlike juce::ProgressBar, which follows
// its value on a timer).
class ExportProgressBar final : public juce::Component
{
public:
    void setFraction (double fraction);
    double fraction() const { return value; }
    void paint (juce::Graphics& g) override;

private:
    double value = 0.0;
};

class ExportPage final : public juce::Component
{
public:
    ExportPage();

    void show (const ExportPageState& state);

    exporting::ExportFormat format() const { return chosenFormat; }
    const juce::File& destination() const { return chosenDestination; }
    // Sets where the file goes, with the chosen format's extension.
    void setDestination (const juce::File& file);
    exporting::ExportRequest request() const { return { chosenFormat, chosenDestination }; }
    // The progress bar's value, 0..1.
    double progress() const { return progressBar.fraction(); }

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Export pressed, with the request.
    std::function<void (exporting::ExportRequest)> onExport;
    // Choose… pressed: asks for a destination, starting from the current one.
    std::function<void (juce::File current)> onChooseDestination;
    // Reveal in Finder pressed, with the file the last export wrote.
    std::function<void (juce::File)> onReveal;

    // Children are public so tests can click them and check what shows.
    juce::Label title;
    juce::Label formatHeading;
    juce::ToggleButton wavButton { juce::String::fromUTF8 ("WAV \xe2\x80\x94 24-bit, Song sample rate") };
    juce::ToggleButton m4aButton { juce::String::fromUTF8 ("M4A \xe2\x80\x94 AAC 256 kbps") };
    juce::ToggleButton mp4Button { juce::String::fromUTF8 ("MP4 \xe2\x80\x94 mix + one Video Take (coming in a later version)") };
    juce::Label spanLabel;
    juce::Label destinationHeading;
    juce::Label destinationLabel;
    juce::TextButton chooseButton { juce::String::fromUTF8 ("Choose\xe2\x80\xa6") };
    juce::TextButton exportButton { "Export" };
    ExportProgressBar progressBar;
    juce::Label statusLabel;
    juce::TextButton revealButton { "Reveal in Finder" };

private:
    void chooseFormat (exporting::ExportFormat format);
    void updateControls();

    exporting::ExportFormat chosenFormat = exporting::ExportFormat::wav;
    juce::File chosenDestination;
    ExportPageState shown;
};
} // namespace overdub::ui
