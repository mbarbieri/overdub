#include "ui/TranscribePage.h"

#include "song/PracticeSpeed.h"
#include "song/Transpose.h"
#include "ui/Colours.h"
#include "ui/Layout.h"
#include "ui/RecordView.h"
#include "ui/TranscribeGeometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace overdub::ui
{
namespace
{
// The mix's columns as a filled shape around the vertical centre of `area`.
void paintColumns (juce::Graphics& g, const std::vector<song::PeakRange>& columns, juce::Rectangle<int> area, juce::Colour colour)
{
    const auto centre = (float) area.getCentreY();
    const auto halfHeight = (float) area.getHeight() * 0.5f;
    g.setColour (colour);
    for (std::size_t i = 0; i < columns.size() && (int) i < area.getWidth(); ++i)
    {
        const auto top = centre - columns[i].maximum * halfHeight;
        const auto bottom = centre - columns[i].minimum * halfHeight;
        g.fillRect ((float) area.getX() + (float) i, top, 1.0f, std::max (1.0f, bottom - top));
    }
}

void styleCaption (juce::Label& caption)
{
    caption.setColour (juce::Label::textColourId, colours::dimText);
    caption.setFont (juce::FontOptions (9.5f, juce::Font::bold));
    caption.setBorderSize ({});
    caption.setInterceptsMouseClicks (false, false);
}

void styleUnit (juce::Label& unit)
{
    unit.setColour (juce::Label::textColourId, colours::dimText);
    unit.setFont (juce::FontOptions (11.0f));
    unit.setBorderSize ({});
    unit.setInterceptsMouseClicks (false, false);
}

void styleField (juce::Label& field)
{
    field.setEditable (true, false, false);
    field.setFont (juce::FontOptions (12.0f));
    field.setJustificationType (juce::Justification::centred);
    field.setColour (juce::Label::textColourId, colours::text);
    field.setColour (juce::Label::outlineColourId, colours::line);
}
} // namespace

//==============================================================================
void TranscribeOverview::show (std::vector<song::PeakRange> newColumns, song::SampleCount newLength, TimelineView newWindow, int newWindowWidth, song::SampleCount newPlayhead)
{
    if (newColumns == columns && newLength == songLength && newWindow == window && newWindowWidth == windowWidth && newPlayhead == playhead)
        return;
    columns = std::move (newColumns);
    songLength = std::max<song::SampleCount> (1, newLength);
    window = newWindow;
    windowWidth = std::max (1, newWindowWidth);
    playhead = newPlayhead;
    repaint();
}

juce::Rectangle<float> TranscribeOverview::windowRectangle() const
{
    const auto width = getWidth();
    const auto from = (float) overviewX (window.firstSample, width, songLength);
    const auto to = (float) overviewX (window.firstSample + window.samplesPerPixel * windowWidth, width, songLength);
    return juce::Rectangle<float>::leftTopRightBottom (from, 0.0f, std::max (to, from + 2.0f), (float) getHeight());
}

void TranscribeOverview::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setColour (colours::panel.darker (0.2f));
    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);
    paintColumns (g, columns, bounds.reduced (0, 4), colours::backingClip.brighter (0.4f));

    const auto rectangle = windowRectangle();
    g.setColour (colours::accent.withAlpha (0.18f));
    g.fillRect (rectangle);
    g.setColour (colours::accent);
    g.drawRect (rectangle, 1.5f);

    g.setColour (juce::Colours::white);
    g.drawVerticalLine (juce::roundToInt (overviewX (playhead, bounds.getWidth(), songLength)), 0.0f, (float) bounds.getHeight());
}

void TranscribeOverview::mouseDown (const juce::MouseEvent& event)
{
    if (onMoveWindow)
        onMoveWindow (event.getPosition().x);
}

void TranscribeOverview::mouseDrag (const juce::MouseEvent& event)
{
    mouseDown (event);
}

//==============================================================================
void TranscribeWave::show (std::vector<song::PeakRange> newColumns, TimelineView newView, int newSampleRate, song::SampleCount newPlayhead)
{
    if (newColumns == columns && newView == view && newSampleRate == sampleRate && newPlayhead == playhead)
        return;
    columns = std::move (newColumns);
    view = newView;
    sampleRate = std::max (1, newSampleRate);
    playhead = newPlayhead;
    repaint();
}

void TranscribeWave::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const auto ruler = bounds.removeFromTop (transcribeRulerHeight);
    g.setColour (colours::panel);
    g.fillRect (ruler);
    g.setColour (colours::panel.darker (0.3f));
    g.fillRect (bounds);
    g.setColour (colours::line);
    g.drawHorizontalLine (ruler.getBottom() - 1, 0.0f, (float) getWidth());
    g.drawHorizontalLine (bounds.getCentreY(), 0.0f, (float) getWidth());

    const auto step = rulerStepSeconds (view.samplesPerPixel, sampleRate);
    const auto firstSecond = (double) view.firstSample / sampleRate;
    const auto lastSecond = (double) sampleForX (view, getWidth()) / sampleRate;
    g.setFont (juce::FontOptions (10.0f));
    for (auto tick = std::floor (firstSecond / step); tick * step <= lastSecond; tick += 1.0)
    {
        const auto seconds = tick * step;
        const auto x = xForSample (view, (song::SampleCount) std::llround (seconds * sampleRate));
        g.setColour (colours::line.brighter (0.3f));
        g.drawVerticalLine (x, (float) ruler.getY(), (float) getHeight());
        g.setColour (colours::dimText);
        g.drawText (juce::String (rulerLabel (seconds)), x + 3, ruler.getY() + 4, 60, 12, juce::Justification::centredLeft);
    }

    paintColumns (g, columns, bounds.reduced (0, 8), colours::backingClip.brighter (0.4f));

    const auto x = xForSample (view, playhead);
    if (x >= 0 && x < getWidth())
    {
        g.setColour (juce::Colours::white);
        g.drawVerticalLine (x, 0.0f, (float) getHeight());
    }
}

void TranscribeWave::seekAt (int x)
{
    if (onSeek)
        onSeek (std::max<song::SampleCount> (0, sampleForX (view, std::max (0, x))));
}

void TranscribeWave::mouseDown (const juce::MouseEvent& event)
{
    seekAt (event.getPosition().x);
}

void TranscribeWave::mouseDrag (const juce::MouseEvent& event)
{
    seekAt (event.getPosition().x);
}

void TranscribeWave::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCommandDown())
    {
        if (wheel.deltaY != 0.0f && onZoom)
            onZoom (transcribeWheelZoomScale (wheel.deltaY), event.x);
        return;
    }
    const auto pixels = transcribeWheelPixels (wheel.deltaX, wheel.deltaY);
    if (pixels != 0 && onScroll)
        onScroll (pixels);
}

void TranscribeWave::mouseMagnify (const juce::MouseEvent& event, float scaleFactor)
{
    if (onZoom)
        onZoom (scaleFactor, event.x);
}

//==============================================================================
TranscribeColumn::TranscribeColumn()
{
    heading.setColour (juce::Label::textColourId, colours::text);
    heading.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    heading.setBorderSize ({});
    heading.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (heading);
    for (auto* caption : { &practiceSpeedCaption, &transposeCaption })
    {
        styleCaption (*caption);
        addAndMakeVisible (caption);
    }
    for (auto* unit : { &practiceSpeedUnit, &transposeUnit })
    {
        styleUnit (*unit);
        addAndMakeVisible (unit);
    }
    for (auto* field : { &practiceSpeedField, &transposeField })
    {
        styleField (*field);
        addAndMakeVisible (field);
    }
    practiceSpeedField.setText (juce::String (shownPracticeSpeed), juce::dontSendNotification);
    practiceSpeedField.onTextChange = [this] { practiceSpeedEntered(); };
    transposeField.setText (shownTranspose, juce::dontSendNotification);
    transposeField.onTextChange = [this] { transposeEntered(); };
    for (auto* spinner : { &practiceSpeedSpinner, &transposeSpinner })
    {
        spinner->setInterceptsMouseClicks (false, false);
        addChildComponent (spinner);
    }
}

void TranscribeColumn::practiceSpeedEntered()
{
    const auto typed = parsePracticeSpeed (practiceSpeedField.getText().toStdString());
    // The field shows the open Song's value until the handler has changed it (show() is called back).
    practiceSpeedField.setText (juce::String (shownPracticeSpeed), juce::dontSendNotification);
    if (typed.has_value() && onIntent)
        onIntent (SongIntent { intent::SetPracticeSpeed { *typed } });
}

void TranscribeColumn::transposeEntered()
{
    const auto typed = parseTranspose (transposeField.getText().toStdString());
    transposeField.setText (juce::String::fromUTF8 (shownTranspose.c_str()), juce::dontSendNotification);
    if (typed.has_value() && onIntent)
        onIntent (SongIntent { intent::SetTranspose { *typed } });
}

void TranscribeColumn::show (const TranscribePageState& state)
{
    shownPracticeSpeed = state.practiceSpeed;
    if (! practiceSpeedField.isBeingEdited())
        practiceSpeedField.setText (juce::String (shownPracticeSpeed), juce::dontSendNotification);
    practiceSpeedField.setColour (juce::Label::backgroundColourId, state.practiceSpeed != song::normalPracticeSpeed ? colours::accent : colours::background);
    practiceSpeedField.setEnabled (state.canSetPracticeSpeed);
    practiceSpeedSpinner.setVisible (state.practiceRendering);

    shownTranspose = song::transposeText (state.transpose);
    if (! transposeField.isBeingEdited())
        transposeField.setText (juce::String::fromUTF8 (shownTranspose.c_str()), juce::dontSendNotification);
    transposeField.setColour (juce::Label::backgroundColourId, state.transpose != 0 ? colours::accent : colours::background);
    transposeField.setEnabled (state.canTranspose);
    transposeSpinner.setVisible (state.transposeRendering);
}

void TranscribeColumn::resized()
{
    const auto layout = transcribeColumnLayout (getLocalBounds());
    heading.setBounds (layout.heading);
    practiceSpeedCaption.setBounds (layout.practiceSpeedCaption);
    practiceSpeedField.setBounds (layout.practiceSpeedField);
    practiceSpeedUnit.setBounds (layout.practiceSpeedUnit);
    practiceSpeedSpinner.setBounds (layout.practiceSpeedSpinner);
    transposeCaption.setBounds (layout.transposeCaption);
    transposeField.setBounds (layout.transposeField);
    transposeUnit.setBounds (layout.transposeUnit);
    transposeSpinner.setBounds (layout.transposeSpinner);
}

void TranscribeColumn::paint (juce::Graphics& g)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
}

//==============================================================================
TranscribePage::TranscribePage (PeaksLookup peaksLookup)
    : peaks (std::move (peaksLookup))
{
    overview.onMoveWindow = [this] (int x) {
        const auto current = view();
        const auto start = overviewViewStart (x, overview.getWidth(), songLength(), current.samplesPerPixel * waveformWidth());
        emit (SongIntent { intent::SetTranscribeView { start, current.samplesPerPixel } });
    };
    wave.onSeek = [this] (song::SampleCount sample) { emit (SongIntent { intent::Seek { sample } }); };
    wave.onScroll = [this] (int pixels) {
        const auto current = view();
        emit (SongIntent { intent::SetTranscribeView { current.firstSample + (song::SampleCount) pixels * current.samplesPerPixel, current.samplesPerPixel } });
    };
    wave.onZoom = [this] (float scaleFactor, int x) {
        const auto current = view();
        const auto limits = transcribeZoomLimits (shown.song.sampleRate, songLength(), waveformWidth());
        const auto pinch = pinchZoomed (current, pinchPendingLevels, scaleFactor, x, limits.minimum, limits.maximum);
        pinchPendingLevels = pinch.pendingLevels;
        emit (SongIntent { intent::SetTranscribeView { pinch.view.firstSample, pinch.view.samplesPerPixel } });
    };
    column.onIntent = [this] (const Intent& intent) { emit (intent); };

    addAndMakeVisible (overview);
    addAndMakeVisible (wave);
    addAndMakeVisible (column);
}

void TranscribePage::emit (const Intent& intent) const
{
    if (onIntent)
        onIntent (intent);
}

song::SampleCount TranscribePage::songLength() const
{
    return overviewLength (shown.song, shown.playhead);
}

int TranscribePage::waveformWidth() const
{
    return std::max (1, wave.getWidth());
}

TimelineView TranscribePage::view() const
{
    const auto width = waveformWidth();
    const auto limits = transcribeZoomLimits (shown.song.sampleRate, songLength(), width);
    return clampedTranscribeView (requested, limits.minimum, limits.maximum, songLength(), width);
}

void TranscribePage::setView (song::SampleCount firstSample, song::SampleCount samplesPerPixel)
{
    requested.firstSample = firstSample;
    requested.samplesPerPixel = samplesPerPixel;
    requested = view();
    showViews();
}

void TranscribePage::show (const TranscribePageState& state)
{
    const auto columnChanged = state.columnVisible != shown.columnVisible;
    shown = state;
    column.show (state);
    if (columnChanged)
        resized(); // calls showViews
    else
        showViews();
}

void TranscribePage::showViews()
{
    const auto current = view();
    const auto width = waveformWidth();
    if (shown.song != columnsSong || current != columnsView || width != columnsWidth)
    {
        waveColumns = heardMixColumns (shown.song, peaks, current.firstSample, current.samplesPerPixel, width);
        columnsView = current;
        columnsWidth = width;
    }
    const auto length = songLength();
    const auto overviewWidth = std::max (1, overview.getWidth());
    if (shown.song != columnsSong || overviewWidth != overviewColumnsWidth || length != overviewColumnsLength)
    {
        const auto perPixel = std::max<song::SampleCount> (1, (length + overviewWidth - 1) / overviewWidth);
        overviewColumns = heardMixColumns (shown.song, peaks, 0, perPixel, overviewWidth);
        overviewColumnsWidth = overviewWidth;
        overviewColumnsLength = length;
    }
    columnsSong = shown.song;
    wave.show (waveColumns, current, shown.song.sampleRate, shown.playhead);
    overview.show (overviewColumns, length, current, width, shown.playhead);
}

void TranscribePage::resized()
{
    const auto layout = transcribePageLayout (getLocalBounds(), shown.columnVisible);
    overview.setBounds (layout.overview);
    wave.setBounds (layout.ruler.getUnion (layout.waveform));
    column.setBounds (layout.column);
    column.setVisible (shown.columnVisible);
    showViews();
}

void TranscribePage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}
} // namespace overdub::ui
