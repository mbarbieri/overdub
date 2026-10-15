#include "song/TrackColours.h"

#include "core/Error.h"

#include <algorithm>
#include <cctype>
#include <random>

namespace overdub::song
{
PickIndex randomPick()
{
    return [generator = std::mt19937 { std::random_device {}() }] (std::size_t count) mutable {
        return std::uniform_int_distribution<std::size_t> { 0, count - 1 }(generator);
    };
}

bool isTrackColour (std::string_view colour)
{
    const auto isHexDigit = [] (char c) { return std::isxdigit (static_cast<unsigned char> (c)) != 0; };
    return colour.size() == 7 && colour.front() == '#' && std::ranges::all_of (colour.substr (1), isHexDigit);
}

std::string newTrackColour (const std::vector<std::string>& taken, const PickIndex& pick)
{
    std::vector<std::string_view> choices;
    for (const auto colour : trackColourPalette)
        if (! std::ranges::contains (taken, colour))
            choices.push_back (colour);
    if (choices.empty())
        choices.assign (trackColourPalette.begin(), trackColourPalette.end());

    const auto index = pick (choices.size());
    if (! OVERDUB_CHECK (index < choices.size(), "song.track_colour_pick_out_of_range", { "index", index }, { "count", choices.size() }))
        return std::string (choices.front());
    return std::string (choices[index]);
}
} // namespace overdub::song
