#include "ui/LookAheadStrip.h"

#include "ui/Colours.h"

#include <cmath>

namespace overdub::ui
{
namespace
{
constexpr int headerHeight = 18;
constexpr int tickSeconds = 5;

void drawBadge (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification justification, juce::Colour background)
{
    const juce::Font font { juce::FontOptions (12.0f) };
    const auto width = juce::GlyphArrangement::getStringWidthInt (font, text) + 14;
    auto badge = area.withHeight (20);
    badge = justification.testFlags (juce::Justification::right) ? badge.removeFromRight (width) : badge.removeFromLeft (width);
    g.setColour (background);
    g.fillRoundedRectangle (badge.toFloat(), 4.0f);
    g.setColour (colours::text);
    g.setFont (font);
    g.drawText (text, badge, juce::Justification::centred);
}
} // namespace

LookAheadStrip::LookAheadStrip()
{
    setInterceptsMouseClicks (false, false);
}

void LookAheadStrip::show (const LookAheadView& newView, int sampleRate)
{
    if (newView == view && sampleRate == rate)
        return;
    view = newView;
    rate = sampleRate;
    repaint();
}

juce::Rectangle<int> LookAheadStrip::waveformBounds() const
{
    return getLocalBounds().withTrimmedTop (headerHeight);
}

void LookAheadStrip::paint (juce::Graphics& g)
{
    const auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (colours::dimText);
    g.setFont (juce::FontOptions (10.5f));
    g.drawText (juce::String::fromUTF8 ("BACKING TRACK \xC2\xB7 WHAT'S COMING"), header, juce::Justification::centredLeft);
    g.drawText ("next " + juce::String (juce::roundToInt (lookAheadWindowSeconds * (1.0 - lookAheadPlayheadFraction))) + " s", header, juce::Justification::centredRight);

    const auto area = waveformBounds();
    const auto width = area.getWidth();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
    if (width <= 0 || view.window.length <= 0)
        return;

    const auto xOf = [&] (SampleCount sample) { return (float) area.getX() + (float) lookAheadX (view.window, sample, width); };
    const auto top = (float) area.getY();
    const auto bottom = (float) area.getBottom();
    const auto middle = (float) area.getCentreY();
    const auto halfHeight = (float) area.getHeight() * 0.5f - 3.0f;

    // Seconds ticks, every 5 s of Song time.
    g.setFont (juce::FontOptions (9.5f));
    const auto firstTick = (int) std::ceil ((double) view.window.start / rate / tickSeconds) * tickSeconds;
    for (auto second = std::max (firstTick, 0); (SampleCount) second * rate < view.window.start + view.window.length; second += tickSeconds)
    {
        const auto x = xOf ((SampleCount) second * rate);
        g.setColour (colours::line);
        g.drawVerticalLine ((int) x, top, bottom);
        g.setColour (colours::dimText);
        g.drawText (juce::String (second) + " s", juce::Rectangle<float> (x + 3.0f, top + 2.0f, 40.0f, 12.0f), juce::Justification::centredLeft);
    }

    // The envelope: one centred bar per column, its height the square root of the level so quiet
    // passages stay visible.
    g.setColour (colours::envelope);
    for (std::size_t column = 0; column < view.columns.size() && (int) column < width; ++column)
    {
        const auto level = std::sqrt (std::min (view.columns[column], 1.0f));
        if (level <= 0.0f)
            continue;
        const auto x = (float) area.getX() + (float) column;
        g.drawVerticalLine ((int) x, middle - level * halfHeight, middle + level * halfHeight);
    }

    const auto playheadX = xOf (view.window.start + (SampleCount) std::llround ((double) view.window.length * lookAheadPlayheadFraction));
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom ((float) area.getX(), top, playheadX, bottom));

    if (view.passStart.has_value())
    {
        const auto from = std::max ((float) area.getX(), xOf (*view.passStart));
        g.setColour (colours::rec.withAlpha (0.18f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (from, top, playheadX, bottom));
        g.setColour (colours::rec);
        g.drawVerticalLine ((int) from, top, bottom);
    }
    if (view.recordPoint.has_value())
    {
        g.setColour (colours::rec);
        const auto x = xOf (*view.recordPoint);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, top, 2.0f, bottom - top));
    }

    g.setColour (view.passStart.has_value() ? colours::rec : juce::Colours::white);
    g.fillRect (juce::Rectangle<float> (playheadX - 1.0f, top, 2.0f, bottom - top));

    // Along the bottom, clear of the seconds labels at the top.
    const auto badges = area.reduced (8, 6).removeFromBottom (20);
    drawBadge (g, juce::String::fromUTF8 (view.countdown.c_str()), badges, juce::Justification::right, juce::Colours::black.withAlpha (0.65f));
    if (! view.preRollText.empty())
        drawBadge (g, juce::String::fromUTF8 (view.preRollText.c_str()), badges.withTrimmedLeft ((int) playheadX - area.getX() + 6), juce::Justification::left, colours::rec.withAlpha (0.85f));
}

//==============================================================================

SongOverview::SongOverview()
{
    setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void SongOverview::show (const song::Song& song, SampleCount newPlayhead)
{
    std::vector<std::vector<ClipBar>> newRows;
    for (const auto& track : song.tracks)
    {
        auto& row = newRows.emplace_back();
        for (const auto& clip : track.clips)
            row.push_back ({ clip.timelineStart, clip.timelineStart + clip.length, clip.source.kind == song::ClipSourceKind::backing });
    }
    const auto newLength = overviewLength (song, newPlayhead);
    if (newRows == rows && newLength == songLength && newPlayhead == playhead)
        return;
    rows = std::move (newRows);
    songLength = newLength;
    playhead = newPlayhead;
    repaint();
}

void SongOverview::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    g.setColour (colours::panel.darker (0.2f));
    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

    const auto width = bounds.getWidth();
    if (! rows.empty())
    {
        const auto rowHeight = std::clamp ((bounds.getHeight() - 6) / (int) rows.size(), 2, 9);
        auto y = 3;
        for (const auto& row : rows)
        {
            for (const auto& bar : row)
            {
                const auto from = (float) overviewX (bar.start, width, songLength);
                const auto to = (float) overviewX (bar.end, width, songLength);
                g.setColour (bar.backing ? colours::backingClip.brighter (0.2f) : colours::takeClip.brighter (0.3f));
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (from, (float) y + 1.0f, std::max (to, from + 1.0f), (float) (y + rowHeight) - 1.0f));
            }
            y += rowHeight;
        }
    }

    g.setColour (juce::Colours::white);
    g.drawVerticalLine (juce::roundToInt (overviewX (playhead, width, songLength)), 0.0f, (float) bounds.getHeight());
}

void SongOverview::mouseDown (const juce::MouseEvent& event)
{
    if (onSeek)
        onSeek (overviewSampleAt (event.getPosition().x, getWidth(), songLength));
}
} // namespace overdub::ui
