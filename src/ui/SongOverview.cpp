#include "ui/SongOverview.h"

#include "ui/Colours.h"

#include <algorithm>

namespace overdub::ui
{
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
