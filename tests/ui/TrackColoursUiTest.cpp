#include "ui/Colours.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

// A Track's colour (issue #137) through the real window: the Record row, the Edit header, the
// Track's Clips, its Take Lanes (dimmed) and its Mixer strip all take the colour stored in the
// Song. Colours are set by hand, away from the palette, so a stale or default colour cannot pass.
using namespace overdub::ui;
using namespace overdub::ui::testing;
namespace song = overdub::song;

namespace
{
// One Pass recorded on Guitar and Vocals, then Guitar #c0ffee, Vocals #f4a6a6 and Band Mix
// #123456 as one undo step.
struct TrackColourFixture : MainWindowFixture
{
    TrackColourFixture()
    {
        REQUIRE (pressKey (*window, 'r'));
        runBlocks (300);
        REQUIRE (pressKey (*window, juce::KeyPress::spaceKey));
        runBlocks (1);
        REQUIRE (! song().takes.empty());

        auto coloured = song();
        for (auto& track : coloured.tracks)
            track.colour = track.name == "Guitar" ? "#c0ffee" : (track.name == "Vocals" ? "#f4a6a6" : "#123456");
        REQUIRE (openSong->edit (coloured, "Colour Tracks").has_value());
        window->tick();
    }

    const song::Take& takeOn (const std::string& trackName)
    {
        const auto& trackId = trackNamed (trackName).id;
        const auto found = std::ranges::find (song().takes, trackId, &song::Take::trackId);
        REQUIRE (found != song().takes.end());
        return *found;
    }

    MixerStrip& stripNamed (const std::string& trackName)
    {
        const auto& trackId = trackNamed (trackName).id;
        for (auto& strip : window->editPage.mixer.strips)
            if (strip->trackId() == trackId)
                return *strip;
        FAIL ("no Mixer strip for Track " << trackName);
        return *window->editPage.mixer.strips.front();
    }
};
} // namespace

TEST_CASE ("a Track's colour is the Song's \"#rrggbb\", opaque, and its Take Lanes' is each channel at 60 %", "[ui][colour]")
{
    CHECK (trackColour (song::Track { .colour = "#c0ffee" }) == juce::Colour (0xffc0ffee));
    CHECK (trackColour (song::Track { .colour = "#A8D8B9" }) == juce::Colour (0xffa8d8b9));
    CHECK (takeLaneColour (song::Track { .colour = "#c0ffee" }) == juce::Colour (0xff73998f));
    CHECK (takeLaneColour (song::Track { .colour = "#a8d8b9" }) == juce::Colour (0xff65826f));
    CHECK (trackColour (song::Track { .colour = "mint" }) == colours::dimText);
}

TEST_CASE ("the Record rows compute each Track's colour", "[ui][record_page][colour]")
{
    TrackColourFixture f;

    const auto state = recordPageStateFor (f.song(), {}, [] (const song::Id&) { return false; }, false);
    REQUIRE (state.rows.size() == f.song().tracks.size());
    for (std::size_t i = 0; i < state.rows.size(); ++i)
        CHECK (state.rows[i].colour == trackColour (f.song().tracks[i]));

    CHECK (f.rowNamed ("Guitar").colour() == juce::Colour (0xffc0ffee));
    CHECK (f.rowNamed ("Vocals").colour() == juce::Colour (0xfff4a6a6));
    CHECK (f.rowNamed ("Band Mix").colour() == juce::Colour (0xff123456));
}

TEST_CASE ("the Edit headers, Clips, Take Lanes and Mixer strips compute each Track's colour, dimmed for Take Lanes", "[ui][edit_page][colour]")
{
    TrackColourFixture f;
    REQUIRE (pressKey (*f.window, '2'));
    auto& timeline = f.window->editPage.timeline;
    timeline.setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    timeline.setTakeLanesUnfolded (f.trackNamed ("Vocals").id, true);

    CHECK (f.editHeaderNamed ("Guitar").colour() == juce::Colour (0xffc0ffee));
    CHECK (f.editHeaderNamed ("Vocals").colour() == juce::Colour (0xfff4a6a6));
    CHECK (f.editHeaderNamed ("Band Mix").colour() == juce::Colour (0xff123456));

    CHECK (timeline.clipColour (f.trackNamed ("Guitar").clips.at (0).id) == juce::Colour (0xffc0ffee));
    CHECK (timeline.clipColour (f.trackNamed ("Vocals").clips.at (0).id) == juce::Colour (0xfff4a6a6));
    CHECK (timeline.clipColour (f.trackNamed ("Band Mix").clips.at (0).id) == juce::Colour (0xff123456)); // a Backing Track Clip
    CHECK (timeline.clipColour ("no-such-clip") == std::nullopt);

    CHECK (timeline.takeLaneColourOf (f.takeOn ("Guitar").id) == juce::Colour (0xff73998f));
    CHECK (timeline.takeLaneColourOf (f.takeOn ("Vocals").id) == juce::Colour (0xff926464));

    CHECK (f.stripNamed ("Guitar").colour() == juce::Colour (0xffc0ffee));
    CHECK (f.stripNamed ("Band Mix").colour() == juce::Colour (0xff123456));
    CHECK (f.window->editPage.mixer.masterStrip.colour() == std::nullopt);
}

TEST_CASE ("undoing a colour change shows the earlier colour everywhere", "[ui][colour]")
{
    TrackColourFixture f;
    const auto& guitar = f.trackNamed ("Guitar");
    REQUIRE (guitar.colour == "#c0ffee");

    REQUIRE (pressKey (*f.window, 'z', juce::ModifierKeys::commandModifier));
    f.window->tick();

    const auto before = trackColour (f.trackNamed ("Guitar"));
    CHECK (before != juce::Colour (0xffc0ffee));
    CHECK (f.rowNamed ("Guitar").colour() == before);
    REQUIRE (pressKey (*f.window, '2'));
    CHECK (f.editHeaderNamed ("Guitar").colour() == before);
    CHECK (f.stripNamed ("Guitar").colour() == before);
}

// build/ui-snapshots/issue-137/*.png, for a session to see the colour stripes, Clips, Take Lanes
// and Mixer strips.
TEST_CASE ("snapshots of Track colours on the Record and Edit pages", "[ui][colour][snapshot]")
{
    TrackColourFixture f;
    auto& window = *f.window;
    CHECK (writeSnapshot (window, "issue-137", "record-rows").existsAsFile());

    REQUIRE (pressKey (window, '2'));
    auto& timeline = window.editPage.timeline;
    timeline.setTakeLanesUnfolded (f.trackNamed ("Guitar").id, true);
    timeline.selectClip (f.trackNamed ("Vocals").clips.at (0).id);
    window.tick();
    CHECK (writeSnapshot (window, "issue-137", "edit-page").existsAsFile());
}

TEST_CASE ("Add Track gives the new Track a palette colour no other Track has", "[ui][record_page][add_track][colour]")
{
    MainWindowFixture f;
    click (f.window->recordPage.addTrackButton);

    const auto& added = f.song().tracks.back();
    CHECK (std::ranges::contains (song::trackColourPalette, std::string_view (added.colour)));
    const auto others = std::ranges::count (f.song().tracks, added.colour, &song::Track::colour);
    CHECK (others == 1);
    CHECK (f.rowNamed (added.name).colour() == trackColour (added));
}
