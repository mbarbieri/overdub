#include "ui/LastSong.h"

#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using overdub::core::AbsolutePath;
using overdub::core::ScopedLogCapture;
using overdub::ui::AppIntent;
using overdub::ui::lastSongFileFor;
using overdub::ui::readLastSong;
using overdub::ui::writeLastSong;
using overdub::ui::testing::contains;
using overdub::ui::testing::eventsNamed;
using overdub::ui::testing::messageCodes;
using overdub::ui::testing::WorkspaceFixture;
using namespace overdub::ui::intent;

// The Last Song (issue #206): the Song most recently opened successfully, recorded in a
// per-machine file and reopened by reopen_last_song, which launch runs through MainComponent::run.
namespace
{
AbsolutePath pathIn (const WorkspaceFixture& f, const std::string& name)
{
    return AbsolutePath { f.folder.getChildFile (name).getFullPathName().toStdString() };
}

juce::File fileOf (const AbsolutePath& path)
{
    return juce::File (juce::String::fromUTF8 (path.value.c_str()));
}

// A fixture whose Workspace records the Last Song in its temp folder.
struct LastSongFixture : WorkspaceFixture
{
    LastSongFixture() { files.lastSong = folder.getChildFile ("last-song.json"); }

    // Runs an App Intent that opens a Song, then one window tick, after which it has finished opening.
    void openAndTick (const AppIntent& intent)
    {
        REQUIRE (run (intent).has_value());
        tick();
    }

    // Quits and launches again with the same per-machine files, as the app does.
    void relaunch()
    {
        workspace.reset();
        launch();
    }

    std::optional<juce::File> recorded() const
    {
        auto read = readLastSong (files.lastSong);
        REQUIRE (read.has_value());
        return *read;
    }

    juce::File shown() const { return content.currentSong() != nullptr ? content.currentSong()->package().location() : juce::File(); }
};
} // namespace

TEST_CASE ("new_song_at, open_song_at and duplicate_song_to each make the Song they open the Last Song", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    CHECK_FALSE (f.recorded().has_value());

    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    CHECK (f.recorded() == fileOf (pathIn (f, "First.overdub")));

    f.openAndTick (AppIntent { DuplicateSongTo { pathIn (f, "Copy.overdub") } });
    CHECK (f.recorded() == fileOf (pathIn (f, "Copy.overdub")));

    f.openAndTick (AppIntent { OpenSongAt { pathIn (f, "First.overdub") } });
    CHECK (f.recorded() == fileOf (pathIn (f, "First.overdub")));
}

TEST_CASE ("a Song whose open fails is not recorded: the Last Song stays the one before", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });

    REQUIRE_FALSE (f.run (AppIntent { OpenSongAt { pathIn (f, "Missing.overdub") } }).has_value());
    REQUIRE_FALSE (f.run (AppIntent { NewSongAt { pathIn (f, "First.overdub") } }).has_value());
    f.tick();

    CHECK (f.recorded() == fileOf (pathIn (f, "First.overdub")));
}

TEST_CASE ("a Song is recorded only at the window tick after it opened, so a crash while it opens never makes it the Last Song", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });

    REQUIRE (f.run (AppIntent { NewSongAt { pathIn (f, "Second.overdub") } }).has_value());
    CHECK (f.recorded() == fileOf (pathIn (f, "First.overdub")));

    f.tick();
    CHECK (f.recorded() == fileOf (pathIn (f, "Second.overdub")));
}

TEST_CASE ("reopen_last_song at the next launch opens the Last Song as open_song_at does and logs ui.last_song_reopened", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    f.relaunch();
    REQUIRE (f.content.currentSong() == nullptr);
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE (result.has_value());
    CHECK (f.shown() == fileOf (pathIn (f, "First.overdub")));
    CHECK (f.content.mode() == overdub::ui::Mode::record);
    CHECK (f.song().position() == 0);
    const auto reopened = eventsNamed (capture, "ui.last_song_reopened");
    REQUIRE (reopened.size() == 1);
    CHECK (reopened[0]["lvl"] == "info");
    CHECK (reopened[0]["path"] == pathIn (f, "First.overdub").value);
    const auto ran = eventsNamed (capture, "ui.intent_run");
    REQUIRE (ran.size() == 1);
    CHECK (ran[0]["op"] == "reopen_last_song");
}

TEST_CASE ("with no Last Song recorded, reopen_last_song opens nothing and presents nothing", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_last_song");
    CHECK (f.content.currentSong() == nullptr);
    CHECK (overdub::ui::testing::isShown (f.content.noSongPage));
    CHECK (messageCodes (capture).empty());
    CHECK (eventsNamed (capture, "ui.last_song_reopened").empty());
}

TEST_CASE ("a Last Song that is missing says where it was, stays the Last Song and reopens once it is back", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    const auto package = fileOf (pathIn (f, "First.overdub"));
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    f.relaunch();
    const auto elsewhere = f.folder.getChildFile ("Unmounted.overdub");
    REQUIRE (package.moveFileTo (elsewhere));
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.last_song_missing");
    CHECK (result.error().fields.at ("path") == package.getFullPathName().toStdString());
    CHECK (result.error().fields.at ("name") == "First");
    const auto missing = eventsNamed (capture, "ui.last_song_missing");
    REQUIRE (missing.size() == 1);
    CHECK (missing[0]["lvl"] == "warn");
    CHECK (missing[0]["path"] == package.getFullPathName().toStdString());
    CHECK (messageCodes (capture) == std::vector<std::string> { "ui.last_song_missing" });
    CHECK (f.content.currentSong() == nullptr);
    CHECK (overdub::ui::testing::isShown (f.content.noSongPage));
    CHECK (f.recorded() == package);

    f.relaunch();
    REQUIRE (elsewhere.moveFileTo (package));
    REQUIRE (f.run (AppIntent { ReopenLastSong {} }).has_value());
    CHECK (f.shown() == package);
}

TEST_CASE ("a Last Song that fails to open says why, as File > Open would, and is forgotten", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    const auto package = fileOf (pathIn (f, "First.overdub"));
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    f.relaunch();
    // Saved by a newer Overdub.
    const auto songJson = package.getChildFile ("song.json");
    auto document = nlohmann::json::parse (songJson.loadFileAsString().toStdString());
    document["formatVersion"] = 9999;
    REQUIRE (songJson.replaceWithText (document.dump()));
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.last_song_failed");
    CHECK (result.error().fields.at ("code") == "song.format_too_new");
    CHECK (result.error().fields.at ("name") == "First");
    const auto failed = eventsNamed (capture, "ui.last_song_failed");
    REQUIRE (failed.size() == 1);
    CHECK (failed[0]["lvl"] == "warn");
    CHECK (failed[0]["path"] == package.getFullPathName().toStdString());
    CHECK (failed[0]["code"] == "song.format_too_new");
    CHECK (messageCodes (capture) == std::vector<std::string> { "ui.last_song_failed" });
    CHECK (f.content.currentSong() == nullptr);
    CHECK_FALSE (f.recorded().has_value());

    f.relaunch();
    ScopedLogCapture next;
    const auto again = f.run (AppIntent { ReopenLastSong {} });
    REQUIRE_FALSE (again.has_value());
    CHECK (again.error().code == "ui.no_last_song");
    CHECK (messageCodes (next).empty());
    CHECK (overdub::ui::testing::isShown (f.content.noSongPage));
}

TEST_CASE ("a Last Song at a rate the device does not offer opens without audio and says so, as open_song_at does", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    f.machine.deviceRate = 44100;
    f.machine.offeredRates = { 44100 };
    f.relaunch();
    ScopedLogCapture capture;

    REQUIRE (f.run (AppIntent { ReopenLastSong {} }).has_value());

    CHECK (f.shown() == fileOf (pathIn (f, "First.overdub")));
    REQUIRE (f.song().audioProblem().has_value());
    CHECK (f.song().audioProblem()->code == "ui.sample_rate_mismatch");
    CHECK (contains (messageCodes (capture), "ui.sample_rate_mismatch"));
}

TEST_CASE ("a Last Song with no audio device opens without audio, as open_song_at does", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    f.launch();
    f.openAndTick (AppIntent { NewSongAt { pathIn (f, "First.overdub") } });
    f.machine.deviceAvailable = false;
    f.relaunch();

    REQUIRE (f.run (AppIntent { ReopenLastSong {} }).has_value());

    CHECK (f.shown() == fileOf (pathIn (f, "First.overdub")));
    CHECK (f.audio().engine() == nullptr);
}

TEST_CASE ("with no Last Song file (a scripted launch), opening a Song records nothing and reopen_last_song finds none", "[ui][workspace][last_song]")
{
    WorkspaceFixture f;
    REQUIRE (f.files.lastSong == juce::File());
    f.launch();
    REQUIRE (f.run (AppIntent { NewSongAt { pathIn (f, "First.overdub") } }).has_value());
    f.tick();
    CHECK (f.folder.findChildFiles (juce::File::findFiles, false, "*.json").isEmpty());

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_last_song");
}

TEST_CASE ("an unreadable Last Song file is treated as no Last Song, logged once at warn, presenting nothing", "[ui][workspace][last_song]")
{
    LastSongFixture f;
    REQUIRE (f.files.lastSong.replaceWithText ("not json"));
    f.launch();
    ScopedLogCapture capture;

    const auto result = f.run (AppIntent { ReopenLastSong {} });

    REQUIRE_FALSE (result.has_value());
    CHECK (result.error().code == "ui.no_last_song");
    CHECK (eventsNamed (capture, "ui.no_last_song").empty());
    const auto logged = eventsNamed (capture, "ui.last_song_unreadable");
    REQUIRE (logged.size() == 1);
    CHECK (logged[0]["lvl"] == "warn");
    CHECK (logged[0]["path"] == f.files.lastSong.getFullPathName().toStdString());
    CHECK (messageCodes (capture).empty());
    CHECK (f.content.currentSong() == nullptr);
}

TEST_CASE ("readLastSong names an unreadable file; writeLastSong records a package it reads back", "[ui][last_song]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("overdub-last-song-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile ("last-song.json");
    REQUIRE (file.replaceWithText ("not json"));

    const auto read = readLastSong (file);

    REQUIRE_FALSE (read.has_value());
    CHECK (read.error().code == "ui.last_song_unreadable");
    CHECK (read.error().fields.at ("path") == file.getFullPathName().toStdString());

    REQUIRE (writeLastSong (file, folder.getChildFile ("Song.overdub")).has_value());
    const auto written = readLastSong (file);
    REQUIRE (written.has_value());
    CHECK (*written == std::optional (folder.getChildFile ("Song.overdub")));
    folder.deleteRecursively();
}

TEST_CASE ("a launch uses OVERDUB_LAST_SONG_FILE when set, no Last Song when its control socket was moved, else the default", "[ui][last_song]")
{
    const auto defaultFile = overdub::ui::defaultLastSongFile();
    CHECK (defaultFile.getFileName() == "last-song.json");
    CHECK (defaultFile.getParentDirectory() == overdub::engine::defaultDeviceSettingsFile().getParentDirectory());

    CHECK (lastSongFileFor (nullptr, false) == defaultFile);
    CHECK (lastSongFileFor ("", false) == defaultFile);
    CHECK (lastSongFileFor (nullptr, true) == juce::File());
    CHECK (lastSongFileFor ("", true) == juce::File());
    CHECK (lastSongFileFor ("/tmp/run/last-song.json", true) == juce::File ("/tmp/run/last-song.json"));
    CHECK (lastSongFileFor ("/tmp/run/last-song.json", false) == juce::File ("/tmp/run/last-song.json"));
}
