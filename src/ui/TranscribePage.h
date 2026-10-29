#pragma once

#include "song/Peaks.h"
#include "song/Song.h"
#include "ui/Intent.h"
#include "ui/PeaksLookup.h"
#include "ui/TimelineGeometry.h"
#include "ui/TopBar.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

// Transcribe mode's page (issue #225, ADR 0010, prototype/transcribe-mode variant D with C's
// overview): a whole-Song overview strip on top, with the heard mix's waveform and a rectangle for
// the visible window; under it the ruler over one tall, zoomable waveform of the heard mix
// (ui/TranscribeGeometry.h heardMixColumns); and at the right the Transcribe column with Practice
// Speed and Transpose, which set_transcribe_column_visible (I) shows or hides.
//
// Gestures, each an Intent through onIntent (ADR 0008): clicking or dragging the overview moves
// the window there (set_transcribe_view); the wheel scrolls time, and command-wheel or a trackpad
// pinch zooms around the pointer (set_transcribe_view), in until about 2 s fills the width;
// clicking or dragging the ruler or the waveform seeks (seek); a whole number typed into the
// column's fields runs set_practice_speed or set_transpose, as the top bar's do. The view is
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
    void show (std::vector<song::PeakRange> columns, TimelineView view, int sampleRate, song::SampleCount playhead);
    void paint (juce::Graphics& g) override;
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
};

// The right column: "LISTENING", then Practice Speed and Transpose, as the top bar shows them.
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

private:
    void practiceSpeedEntered();
    void transposeEntered();

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
    TranscribeWave wave;
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
