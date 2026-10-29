#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/MarkerStrip.h"
#include "ui/PeaksLookup.h"
#include "ui/TimelineGeometry.h"
#include "ui/TopBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Transcribe mode's page (issue #225, ADR 0010, prototype/transcribe-mode variant D with C's
// overview): a whole-Song overview strip on top, with the heard mix's waveform and a rectangle for
// the visible window; under it the ruler over one tall, zoomable waveform of the heard mix
// (ui/TranscribeGeometry.h transcribeColumns: the Focus Track under its name, or the heard
// Transcribe Mix); and at the right the Transcribe column with Practice Speed, Transpose and the
// Track list of the Focus Track and the Transcribe Mix (issue #226), which
// set_transcribe_column_visible (I) shows or hides.
//
// Gestures, each an Intent through onIntent (ADR 0008): clicking or dragging the overview moves
// the window there (set_transcribe_view); the wheel scrolls time, and command-wheel or a trackpad
// pinch zooms around the pointer (set_transcribe_view), in until about 2 s fills the width;
// clicking or dragging the ruler or the waveform seeks (seek); a whole number typed into the
// column's fields runs set_practice_speed or set_transpose, as the top bar's do; a Track row's ◎
// runs set_focus_track (none on the Focus Track's own ◎), its M set_transcribe_mute and its S
// set_transcribe_solo. The Loop/Marker strip over the ruler and the Markers list in the left
// column (issue #227, ui/MarkerStrip.h) run select_marker, move_marker and rename_marker. The view is
// transient UI state, never in the Song; the page changes it only when the handler calls setView.
namespace overdub::ui
{
struct TranscribePageState
{
    song::Song song;
    song::SampleCount playhead = 0;
    bool columnVisible = true;
    int practiceSpeed = 100;
    bool practiceRendering = false;
    bool canSetPracticeSpeed = false;
    int transpose = 0;
    bool transposeRendering = false;
    bool canTranspose = false;
    // The Focus Track and Transcribe Mix controls are enabled (ui/Availability.h).
    bool canSetTranscribeMix = false;
    // The selected Marker (SongView::selectedMarker), and whether the strip edits Markers.
    std::optional<song::Id> selectedMarker;
    bool markersEditable = false;
};

// The overview strip: the heard mix across the whole Song, the visible window and the playhead.
class TranscribeOverview final : public juce::Component
{
public:
    void show (std::vector<song::PeakRange> columns, song::SampleCount songLength, TimelineView window, int windowWidth, song::SampleCount playhead);
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // The x a click or drag moved the window to.
    std::function<void (int x)> onMoveWindow;

    // The visible window's rectangle, in this component's coordinates.
    juce::Rectangle<float> windowRectangle() const;

private:
    std::vector<song::PeakRange> columns;
    song::SampleCount songLength = 1;
    TimelineView window;
    int windowWidth = 1;
    song::SampleCount playhead = 0;
};

// The ruler over the tall waveform, drawn from the page's view.
class TranscribeWave final : public juce::Component
{
public:
    // `label` names what the columns are (the Focus Track's name, in `labelColour`).
    void show (std::vector<song::PeakRange> columns, TimelineView view, int sampleRate, song::SampleCount playhead, juce::String label, juce::Colour labelColour);
    void paint (juce::Graphics& g) override;
    const juce::String& label() const { return shownLabel; }
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& event, float scaleFactor) override;

    std::function<void (song::SampleCount sample)> onSeek;
    std::function<void (int pixels)> onScroll;
    // A pinch's (or command-wheel's) scale factor around x.
    std::function<void (float scaleFactor, int x)> onZoom;

private:
    void seekAt (int x);

    std::vector<song::PeakRange> columns;
    TimelineView view;
    int sampleRate = 48000;
    song::SampleCount playhead = 0;
    juce::String shownLabel;
    juce::Colour shownLabelColour;
};

// One Track in the Transcribe column's list: its colour and name, ◎ (Focus), M and S of the
// Transcribe Mix. Dimmed when the Transcribe Mix does not sound it.
class TranscribeTrackRow final : public juce::Component
{
public:
    explicit TranscribeTrackRow (song::Id trackId);

    const song::Id& trackId() const { return id; }
    void show (const song::Track& track, bool focused, bool heard, bool enabled);
    void resized() override;
    void paint (juce::Graphics& g) override;

    // The new value a click asks for: focused, muted or soloed.
    std::function<void (bool)> onFocus;
    std::function<void (bool)> onMute;
    std::function<void (bool)> onSolo;

    juce::TextButton focusButton;
    juce::TextButton muteButton { "M" };
    juce::TextButton soloButton { "S" };

private:
    song::Id id;
    juce::String name;
    juce::Colour colour;
    bool focused = false;
    bool muted = false;
    bool soloed = false;
    bool heard = true;
};

// The right column: "LISTENING", then Practice Speed and Transpose, as the top bar shows them,
// then "TRANSCRIBE MIX" over one TranscribeTrackRow per Track in Song order.
class TranscribeColumn final : public juce::Component
{
public:
    TranscribeColumn();

    void show (const TranscribePageState& state);
    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

    juce::Label heading { {}, "LISTENING" };
    juce::Label practiceSpeedCaption { {}, "PRACTICE SPEED" };
    juce::Label practiceSpeedField;
    juce::Label practiceSpeedUnit { {}, "%" };
    Spinner practiceSpeedSpinner;
    juce::Label transposeCaption { {}, "TRANSPOSE" };
    juce::Label transposeField;
    juce::Label transposeUnit { {}, "st" };
    Spinner transposeSpinner;
    juce::Label mixCaption { {}, "TRANSCRIBE MIX" };
    std::vector<std::unique_ptr<TranscribeTrackRow>> trackRows;

private:
    void practiceSpeedEntered();
    void transposeEntered();
    // One row per Track, rebuilt only when the Track ids or their order change.
    void showTracks (const TranscribePageState& state);
    void layOutTrackRows();

    int shownPracticeSpeed = 100;
    std::string shownTranspose = "0";
};

class TranscribePage final : public juce::Component
{
public:
    explicit TranscribePage (PeaksLookup peaksLookup);

    void show (const TranscribePageState& state);
    // Sets the view, clamped to the zoom limits (TranscribeGeometry.h) and the Song.
    void setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel);
    // The view as shown: what setView asked for, clamped to the current Song and width.
    TimelineView view() const;
    bool isColumnVisible() const { return shown.columnVisible; }
    // The heard mix's columns the waveform draws, one per pixel of its width.
    const std::vector<song::PeakRange>& waveformColumns() const { return waveColumns; }

    void resized() override;
    void paint (juce::Graphics& g) override;

    IntentSink onIntent;

    // Children are public so tests can click them (ADR 0005 UI amendment).
    TranscribeOverview overview;
    MarkerStrip markerStrip;
    TranscribeWave wave;
    MarkerList markerList;
    TranscribeColumn column;

private:
    void emit (const Intent& intent) const;
    void showViews();
    song::SampleCount songLength() const;
    int waveformWidth() const;

    PeaksLookup peaks;
    TranscribePageState shown;
    // What setView last asked for; view() clamps it.
    TimelineView requested { 0, defaultSamplesPerPixel, 0 };
    double pinchPendingLevels = 0.0;
    std::vector<song::PeakRange> waveColumns;
    std::vector<song::PeakRange> overviewColumns;
    // What the columns were computed from, so a refresh that changes none of it computes nothing.
    song::Song columnsSong;
    TimelineView columnsView;
    int columnsWidth = -1;
    int overviewColumnsWidth = -1;
    song::SampleCount overviewColumnsLength = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TranscribePage)
};
} // namespace overdub::ui
