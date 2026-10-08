#pragma once

#include "export/ExportFormat.h"
#include "export/MovieSpan.h"
#include "song/Song.h"
#include "video/FrameSelection.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Export mode's page (issues #37 and #38, after the prototype's Export form): the format (WAV,
// M4A, MP4), for an MP4 the Video Take and the length (trimmed to the Video Take, or the full song
// with black where there is no video), the span bar previewing what is exported, where the file
// goes, the Export button, progress, and Reveal in Finder once a file is written. It stays thin:
// the request it builds is handed to onExport, and what it shows comes from show().
//
// The destination keeps its folder and name when the format changes; only its extension follows.
// The Video Take shown is the page's choice once the user picked one, else the Song's stored
// choice (defaultExportVideoTake); a pick is handed to onVideoTakeChosen so the Song stores it.
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
    std::vector<video::VideoTakeSpan> videoTakes; // the Song's, in its order
    std::optional<song::Id> storedVideoTakeId;     // Song::exportVideoTakeId
};

// What the span bar shows, in Song samples.
struct ExportSpanBarState
{
    song::SampleCount scaleEnd = 0;                   // the bar's whole width
    exporting::SampleRange exported;                  // what the file holds
    std::optional<exporting::SampleRange> videoTake;  // MP4: where the Video Take plays
    bool blackOutsideVideo = false;                   // full-song MP4: black around the Video Take

    bool operator== (const ExportSpanBarState&) const = default;
};

// The span bar for this choice. WAV, M4A and an MP4 without a Video Take: the whole mix, from 0
// to the Clips' end. MP4: exporting::movieExportRange over the Clips' end and the Video Take, on a
// scale to the later of the two ends.
ExportSpanBarState exportSpanBar (exporting::ExportFormat format, exporting::VideoLength length, song::SampleCount clipsEnd, const std::optional<video::VideoTakeSpan>& videoTake);
// A range's pixels on a bar `width` wide that spans [0, scaleEnd); empty when the scale is.
juce::Range<int> spanBarPixels (exporting::SampleRange range, song::SampleCount scaleEnd, int width);

// "Whole mix: 0:00.0 – 1:23.4, plus effect tails".
std::string exportSpanText (song::SampleCount clipsEnd, int sampleRate);
// "Trimmed to Pass 2: 0:03.0 – 0:13.0" or "Full song: 0:00.0 – 0:13.0, black outside Pass 2".
std::string movieSpanText (exporting::VideoLength length, const video::VideoTakeSpan& videoTake, exporting::SampleRange exported, int sampleRate);
// "Pass 2 · 0:03.0 – 0:13.0": a Video Take in the picker.
std::string videoTakeChoiceText (const video::VideoTakeSpan& videoTake, int sampleRate);
// The stored choice when it is one of the Video Takes, else the first; none without Video Takes.
std::optional<song::Id> defaultExportVideoTake (const std::vector<video::VideoTakeSpan>& videoTakes, const std::optional<song::Id>& stored);
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

// The span bar: the exported range over the scale, the Video Take on top, black around it for a
// full-song MP4.
class ExportSpanBar final : public juce::Component
{
public:
    void setState (const ExportSpanBarState& state);
    const ExportSpanBarState& state() const { return shown; }
    void paint (juce::Graphics& g) override;

private:
    ExportSpanBarState shown;
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
    // The request Export would make: an MP4's carries the Video Take shown and the length.
    exporting::ExportRequest request() const;
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
    // A Video Take picked for the MP4.
    std::function<void (const song::Id&)> onVideoTakeChosen;

    // Children are public so tests can click them and check what shows.
    juce::Label title;
    juce::Label formatHeading;
    juce::ToggleButton wavButton { juce::String::fromUTF8 ("WAV \xe2\x80\x94 24-bit, Song sample rate") };
    juce::ToggleButton m4aButton { juce::String::fromUTF8 ("M4A \xe2\x80\x94 AAC 256 kbps") };
    juce::ToggleButton mp4Button { juce::String::fromUTF8 ("MP4 \xe2\x80\x94 mix + one Video Take") };
    juce::Label videoTakeHeading;
    juce::ComboBox videoTakePicker;
    juce::Label noVideoTakesLabel;
    juce::Label lengthHeading;
    juce::ToggleButton trimButton { "Trim to the Video Take" };
    juce::ToggleButton fullSongButton { juce::String::fromUTF8 ("Full song, black where there\xe2\x80\x99s no video") };
    ExportSpanBar spanBar;
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
    void chooseLength (exporting::VideoLength length);
    void pickVideoTake();
    // The Video Take shown: the page's pick while it is still in the Song, else the default.
    std::optional<video::VideoTakeSpan> shownVideoTake() const;
    void updatePicker();
    void updateSpan();
    void updateControls();

    exporting::ExportFormat chosenFormat = exporting::ExportFormat::wav;
    exporting::VideoLength chosenLength = exporting::VideoLength::trimToVideoTake;
    std::optional<song::Id> pickedVideoTakeId;
    juce::File chosenDestination;
    ExportPageState shown;
};
} // namespace overdub::ui
