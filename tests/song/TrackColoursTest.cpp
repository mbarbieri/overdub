#include "song/TrackColours.h"

#include "song/BackingTrackImport.h"
#include "song/TrackEdits.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

// Track colours (issue #137): a new Track (Add Track, Import) gets a palette colour no other Track
// has while one is free, else any palette colour; the pick is injected so the choice is fixed.
using namespace overdub::song;

namespace
{
// A pick that returns `index` and records each count it was asked to pick from.
struct RecordingPick
{
    std::size_t index = 0;
    std::vector<std::size_t> counts;

    PickIndex pick()
    {
        return [this] (std::size_t count) {
            counts.push_back (count);
            return index;
        };
    }
};

Song songWithColours (const std::vector<std::string>& colours)
{
    auto song = makeEmptySong (48000);
    for (const auto& colour : colours)
        song.tracks.push_back (Track { .id = "track-" + std::to_string (song.tracks.size()), .name = "Track " + std::to_string (song.tracks.size() + 10), .colour = colour });
    return song;
}

std::vector<std::string> wholePalette()
{
    return { trackColourPalette.begin(), trackColourPalette.end() };
}
} // namespace

TEST_CASE ("an added Track gets a palette colour no other Track has, picked among the free ones in palette order", "[song][tracks][colour]")
{
    const auto song = songWithColours ({ "#a8d8b9", "#a6d4f2", "#123456" }); // palette[0], palette[2], not a palette colour

    RecordingPick first { .index = 0 };
    CHECK (addTrack (song, {}, first.pick()).song.tracks.back().colour == "#f4a6a6"); // palette[1]
    CHECK (first.counts == std::vector<std::size_t> { 10 });

    RecordingPick last { .index = 9 };
    CHECK (addTrack (song, {}, last.pick()).song.tracks.back().colour == "#eeb1e0"); // palette[11]

    RecordingPick third { .index = 2 };
    CHECK (addTrack (song, {}, third.pick()).song.tracks.back().colour == "#d3b5e8"); // palette[4]
}

TEST_CASE ("with every palette colour in use, an added Track still gets a palette colour", "[song][tracks][colour]")
{
    const auto song = songWithColours (wholePalette());

    RecordingPick pick { .index = 4 };
    const auto added = addTrack (song, {}, pick.pick());

    CHECK (added.song.tracks.back().colour == "#d3b5e8"); // palette[4]
    CHECK (pick.counts == std::vector<std::size_t> { 12 });
}

TEST_CASE ("an imported Backing Track's new Track gets a palette colour no other Track has", "[song][import][colour]")
{
    const auto song = songWithColours ({ "#a8d8b9", "#f4a6a6" });
    const BackingTrack backingTrack { .id = "backing-1", .originalFilename = "Band Mix.wav", .originalExtension = "wav", .originalSampleRate = 48000, .channels = 2, .formatName = "WAV", .lengthSamples = 1000 };

    RecordingPick pick { .index = 0 };
    const auto imported = withImportedBackingTrack (song, backingTrack, pick.pick());

    CHECK (imported.tracks.back().name == "Band Mix");
    CHECK (imported.tracks.back().colour == "#a6d4f2"); // palette[2]
    CHECK (pick.counts == std::vector<std::size_t> { 10 });
}

TEST_CASE ("successive added Tracks take different colours until the palette runs out", "[song][tracks][colour]")
{
    auto song = makeEmptySong (48000);
    const auto random = randomPick();
    for (std::size_t i = 0; i < trackColourPalette.size(); ++i)
        song = addTrack (song, {}, random).song;

    auto colours = trackColours (song);
    std::ranges::sort (colours);
    auto palette = wholePalette();
    std::ranges::sort (palette);
    CHECK (colours == palette);

    const auto thirteenth = addTrack (song, {}, random).song.tracks.back().colour;
    CHECK (std::ranges::contains (palette, thirteenth));
}

TEST_CASE ("a stored Track colour is \"#\" and six hex digits", "[song][colour]")
{
    CHECK (isTrackColour ("#a8d8b9"));
    CHECK (isTrackColour ("#C0FFEE"));
    CHECK_FALSE (isTrackColour (""));
    CHECK_FALSE (isTrackColour ("a8d8b9"));
    CHECK_FALSE (isTrackColour ("#a8d8b"));
    CHECK_FALSE (isTrackColour ("#a8d8b9f"));
    CHECK_FALSE (isTrackColour ("#a8d8bg"));
}

TEST_CASE ("every palette colour is a distinct stored Track colour", "[song][colour]")
{
    auto palette = wholePalette();
    for (const auto& colour : palette)
        CHECK (isTrackColour (colour));
    std::ranges::sort (palette);
    CHECK (std::ranges::adjacent_find (palette) == palette.end());
}
