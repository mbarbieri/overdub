#include "ui/MainComponent.h"

#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <string>

using namespace overdub;
using namespace overdub::ui;
using namespace overdub::ui::intent;
using namespace overdub::ui::testing;
using Catch::Matchers::WithinAbs;

// A pending Mixer arrow-key burst is committed, as its own undo step, before any Intent that does
// not run per frame (issue #194).
namespace
{
const song::Track& trackNamed (const song::Song& song, const std::string& name)
{
    const auto found = std::ranges::find (song.tracks, name, &song::Track::name);
    REQUIRE (found != song.tracks.end());
    return *found;
}

// A Song open in Edit mode with two Down presses pending on the Guitar's Mixer fader (-2 dB).
struct SongWithPendingBurst
{
    WorkspaceFixture f;
    song::Id guitarId;

    SongWithPendingBurst()
    {
        f.launch();
        f.openSong();
        // A fixed clock: no poll commits the burst while the test runs.
        const auto now = song::SongPackage::Clock::now();
        f.content.clock = [now] { return now; };
        const plugin::AvailableAudioUnit delay { { "aufx", "dely", "appl" }, "AUDelay", "Apple", "1.0" };
        f.content.listInsertPlugins = [delay] { return std::vector<plugin::AvailableAudioUnit> { delay }; };
        REQUIRE (f.content.run (WindowIntent { ShowMode { Mode::edit } }, Origin::control).has_value());
        guitarId = trackNamed (f.song().song(), "Guitar").id;
        auto& fader = guitarStrip().fader;
        clickAt (fader, fader.thumbCentre());
        REQUIRE (pressKey (f.content, juce::KeyPress (juce::KeyPress::downKey, {}, 0)));
        REQUIRE (pressKey (f.content, juce::KeyPress (juce::KeyPress::downKey, {}, 0)));
        REQUIRE_THAT (guitarVolume(), WithinAbs (0.0, 1.0e-9));
    }

    MixerStrip& guitarStrip()
    {
        for (auto& strip : f.content.songView()->editPage.mixer.strips)
            if (strip->trackId() == guitarId)
                return *strip;
        FAIL ("no Mixer strip for the Guitar");
        return *f.content.songView()->editPage.mixer.strips.front();
    }

    double guitarVolume() { return song::findTrack (f.song().song(), guitarId)->volumeDb; }
};
} // namespace

TEST_CASE ("an export during a pending arrow-key burst starts from the Song with the burst committed as its own undo step", "[ui][intent][mixer][export]")
{
    SongWithPendingBurst s;
    auto& f = s.f;
    REQUIRE (f.content.run (SongIntent { SetExportDestination { core::AbsolutePath { f.folder.getChildFile ("Mix.wav").getFullPathName().toStdString() } } }, Origin::control).has_value());

    REQUIRE (f.content.run (SongIntent { Export {} }, Origin::control).has_value());

    REQUIRE (f.song().isExporting());
    CHECK_THAT (s.guitarVolume(), WithinAbs (-2.0, 1.0e-9));
    CHECK (f.song().package().undoLabel() == "Guitar Volume");
}

TEST_CASE ("add_insert during a pending arrow-key burst comes after the burst's own undo step", "[ui][intent][mixer][inserts]")
{
    SongWithPendingBurst s;
    auto& f = s.f;

    REQUIRE (f.content.run (SongIntent { AddInsert { s.guitarId, "aufx", "dely", "appl" } }, Origin::control).has_value());

    CHECK_THAT (s.guitarVolume(), WithinAbs (-2.0, 1.0e-9));
    REQUIRE (f.content.run (SongIntent { Undo {} }, Origin::control).has_value());
    CHECK (song::findTrack (f.song().song(), s.guitarId)->insertChain.empty());
    CHECK_THAT (s.guitarVolume(), WithinAbs (-2.0, 1.0e-9));
    CHECK (f.song().package().undoLabel() == "Guitar Volume");
}

TEST_CASE ("duplicate_song_to during a pending arrow-key burst writes a copy with the burst committed", "[ui][intent][mixer][app_intent]")
{
    SongWithPendingBurst s;
    auto& f = s.f;
    const auto copy = f.folder.getChildFile ("Copy.overdub");

    REQUIRE (f.content.run (AppIntent { DuplicateSongTo { core::AbsolutePath { copy.getFullPathName().toStdString() } } }, Origin::control).has_value());

    REQUIRE (f.song().package().location() == copy);
    CHECK_THAT (trackNamed (f.song().song(), "Guitar").volumeDb, WithinAbs (-2.0, 1.0e-9));
}

TEST_CASE ("a Window Intent during a pending arrow-key burst commits the burst as its own undo step", "[ui][intent][mixer]")
{
    SongWithPendingBurst s;
    auto& f = s.f;

    REQUIRE (f.content.run (WindowIntent { DismissAlerts {} }, Origin::control).has_value());

    CHECK_THAT (s.guitarVolume(), WithinAbs (-2.0, 1.0e-9));
    CHECK (f.song().package().undoLabel() == "Guitar Volume");
}
