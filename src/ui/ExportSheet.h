#pragma once

#include "export/ExportFormat.h"
#include "export/MovieSpan.h"
#include "song/Song.h"
#include "ui/Intent.h"
#include "video/FrameSelection.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Export sheet (issues #37, #38, #200, #201, after the prototype's Export form): a modal panel
// over the whole main window, dimming it and taking every click, with the format (WAV, M4A, MP4),
// for an MP4 the Video Take, the length (for WAV and M4A the Whole Song, for an MP4 trimmed to the
// Video Take or the full song with black where there is no video, and for every format "Loop:
// <name>" per Loop, issue #235; shown once there is a choice), the span bar previewing what is exported, where the file goes (and
// "Replaces the existing <file name>" when a file is there), Cancel and Export. Export closes it:
// the top bar shows the export's progress and a Notice its file.
//
// It shows the Song's stored choices (Song::export*) and keeps none of its own: show() sets the
// whole form, and every gesture is an Intent reported through onIntent (ADR 0008):
// set_export_format, set_export_length, set_export_video_take, export and show_export { false }
// (Cancel). Choose... is the one hook, onChooseDestination: the window asks for a file, then runs
// set_export_destination. While it shows, a stored destination whose folder is gone is replaced
// by the export folder, and Export writes there (SongView::exportRequest).
namespace overdub::ui
{
struct ExportSheetState
{
    song::SampleCount clipsEnd = 0; // the end of the Song's last Clip
    int sampleRate = 48000;
    std::vector<video::VideoTakeSpan> videoTakes; // the Song's, in its order
    std::vector<song::Loop> loops;                // the Song's, in its order
    exporting::ExportRequest request;             // what Export writes (exportRequestFor)
    bool replacesExisting = false;                // a file is at request.destination
    // The code export is refused with now (ui/Availability.h), none when it may run: Export is
    // unavailable with unavailableReason as its tooltip.
    std::optional<std::string> exportRefusal;
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
// scale to the later of the two ends. A Loop: its range, an MP4's black around the Video Take,
// on a scale to the latest end.
ExportSpanBarState exportSpanBar (exporting::ExportFormat format,
                                  exporting::VideoLength length,
                                  song::SampleCount clipsEnd,
                                  const std::optional<video::VideoTakeSpan>& videoTake,
                                  const std::optional<exporting::SampleRange>& loop = std::nullopt);
// A range's pixels on a bar `width` wide that spans [0, scaleEnd); empty when the scale is.
juce::Range<int> spanBarPixels (exporting::SampleRange range, song::SampleCount scaleEnd, int width);

// "Whole mix: 0:00.0 – 1:23.4, plus effect tails"; "Nothing to export: the Song has no Clips"
// when clipsEnd is 0 (lastClipEnd without Clips), as the export is then refused.
std::string exportSpanText (song::SampleCount clipsEnd, int sampleRate);
// "Trimmed to Pass 2: 0:03.0 – 0:13.0" or "Full song: 0:00.0 – 0:13.0, black outside Pass 2".
std::string movieSpanText (exporting::VideoLength length, const video::VideoTakeSpan& videoTake, exporting::SampleRange exported, int sampleRate);
// "Loop: Solo: 0:03.0 – 0:13.0", and for an MP4 ", black outside Pass 2".
std::string loopSpanText (const song::Loop& loop, const std::optional<video::VideoTakeSpan>& videoTake, int sampleRate);
// "Loop: Solo": a Loop's length choice.
std::string loopChoiceText (const song::Loop& loop);
// "Pass 2 · 0:03.0 – 0:13.0": a Video Take in the picker.
std::string videoTakeChoiceText (const video::VideoTakeSpan& videoTake, int sampleRate);
// The stored choice when it is one of the Video Takes, else the first; none without Video Takes.
std::optional<song::Id> defaultExportVideoTake (const std::vector<video::VideoTakeSpan>& videoTakes, const std::optional<song::Id>& stored);
// Where the Song exports to (issue #201): its stored destination with the format's extension, or
// <exportFolder>/<songName>.<extension> when none is stored, and, with `fallBackWhenFolderGone`
// (it asks the disk), when the stored destination's folder no longer exists.
juce::File exportDestinationFor (const song::Song& song, const juce::File& exportFolder, const std::string& songName, bool fallBackWhenFolderGone);
// What Export writes: the Song's format and length (which only an MP4 uses) to `destination`; an
// MP4 also with the Video Take defaultExportVideoTake picks from `videoTakes` (none without any);
// and the range of the Song's export Loop when it is one of its Loops.
exporting::ExportRequest exportRequestFor (const song::Song& song, const std::vector<video::VideoTakeSpan>& videoTakes, const juce::File& destination);
// "Replaces the existing Band Mix.wav".
std::string replacesExistingText (const juce::File& destination);

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

class ExportSheet final : public juce::Component
{
public:
    ExportSheet();

    void show (const ExportSheetState& state);

    exporting::ExportFormat format() const { return shown.request.format; }
    const juce::File& destination() const { return shown.request.destination; }
    // The request the form shows.
    const exporting::ExportRequest& request() const { return shown.request; }
    bool replacesExisting() const { return shown.replacesExisting; }
    // Whether Export (the button, and Return) is offered: export may run now, a destination is
    // set, and an MP4 has a Video Take to show.
    bool canExport() const;

    void resized() override;
    void paint (juce::Graphics& g) override;

    // Where the sheet's Intents go (MainComponent::run).
    IntentSink onIntent;
    // Choose… pressed: asks for a destination, starting from the current one.
    std::function<void (juce::File current)> onChooseDestination;

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
    juce::ToggleButton wholeSongButton { "Whole Song" }; // WAV and M4A
    juce::ToggleButton trimButton { "Trim to the Video Take" };
    juce::ToggleButton fullSongButton { juce::String::fromUTF8 ("Full song, black where there\xe2\x80\x99s no video") };
    std::vector<std::unique_ptr<juce::ToggleButton>> loopButtons; // one per Loop, in the Song's order
    ExportSpanBar spanBar;
    juce::Label spanLabel;
    juce::Label destinationHeading;
    juce::Label destinationLabel;
    juce::Label replacesLabel;
    juce::TextButton chooseButton { juce::String::fromUTF8 ("Choose\xe2\x80\xa6") };
    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton exportButton { "Export" };

private:
    // The panel in the middle of the sheet, as tall as the form for the chosen format.
    juce::Rectangle<int> panelBounds() const;
    void pickVideoTake();
    // The request's Video Take, when it is one of the Song's.
    std::optional<video::VideoTakeSpan> shownVideoTake() const;
    // The request's Loop, when it is one of the Song's.
    std::optional<song::Loop> shownLoop() const;
    // How many length choices show: Whole Song (WAV, M4A) or trimmed and full song (an MP4 with a
    // Video Take), then one per Loop; none when there is nothing to choose.
    int lengthChoiceCount() const;
    void updatePicker();
    void updateLoopButtons();
    void updateSpan();
    void updateControls();
    void emit (Intent intent) const;

    ExportSheetState shown;
};
} // namespace overdub::ui
