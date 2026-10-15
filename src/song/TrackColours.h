#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Track colours (issue #137): every Track has a pastel colour, chosen when the Track is created.
// The Song stores the colour itself ("#a8d8b9"), never a palette index, so changing the palette
// never recolours an existing Song. ui/Colours.h turns a Track's colour into a juce::Colour.
namespace overdub::song
{
// Pastels that read on the dark background, ordered so that neighbours differ in hue: a migrated
// Song colours its Tracks in this order (song/SongMigration.h).
inline constexpr std::array<std::string_view, 12> trackColourPalette {
    "#a8d8b9", // mint
    "#f4a6a6", // rose
    "#a6d4f2", // sky
    "#f5e19c", // butter
    "#d3b5e8", // lavender
    "#9fdcd4", // teal
    "#f6c39a", // peach
    "#a7b8e8", // cornflower
    "#d4e8a0", // pistachio
    "#f5b3c8", // pink
    "#bcb3ef", // periwinkle
    "#eeb1e0", // orchid
};

// Picks an index in [0, count), count >= 1. The app picks at random (randomPick); tests inject a
// fixed pick.
using PickIndex = std::function<std::size_t (std::size_t count)>;

// A uniform random pick from its own generator, seeded from std::random_device.
PickIndex randomPick();

// Whether `colour` has the stored form: "#" and six hex digits.
bool isTrackColour (std::string_view colour);

// The colour of a new Track: a palette colour none of `taken` is, picked among those still free in
// palette order; when every palette colour is taken, any palette colour.
std::string newTrackColour (const std::vector<std::string>& taken, const PickIndex& pick);
} // namespace overdub::song
