#include "ui/ThinProgressBar.h"

#include "ui/Colours.h"

#include <algorithm>
#include <cmath>

namespace overdub::ui
{
void ThinProgressBar::setFraction (double fraction)
{
    const auto clamped = std::clamp (fraction, 0.0, 1.0);
    if (std::abs (clamped - value) < 1.0e-12)
        return;
    value = clamped;
    repaint();
}

void ThinProgressBar::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const auto radius = area.getHeight() / 2.0f;
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (area, radius);
    if (value <= 0.0)
        return;
    g.setColour (colours::accent);
    g.fillRoundedRectangle (area.withWidth (std::max (area.getHeight(), (float) (area.getWidth() * value))), radius);
}
} // namespace overdub::ui
