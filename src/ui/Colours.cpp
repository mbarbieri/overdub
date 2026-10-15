#include "ui/Colours.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace overdub::ui
{
juce::Colour trackColour (const song::Track& track)
{
    if (! song::isTrackColour (track.colour))
        return colours::dimText;
    std::uint32_t rgb = 0;
    const auto* digits = track.colour.data() + 1;
    std::from_chars (digits, digits + 6, rgb, 16);
    return juce::Colour (0xff000000u | rgb);
}

juce::Colour takeLaneColour (const song::Track& track)
{
    const auto colour = trackColour (track);
    const auto dim = [] (juce::uint8 channel) { return (juce::uint8) std::lround (channel * 0.6); };
    return juce::Colour::fromRGB (dim (colour.getRed()), dim (colour.getGreen()), dim (colour.getBlue()));
}
} // namespace overdub::ui
