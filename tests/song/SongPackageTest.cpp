#include "song/SongPackage.h"
#include "song/PluginBlobStore.h"
#include "song/SongJson.h"

#include "FullSong.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using overdub::core::ScopedLogCapture;
using overdub::song::PackageOptions;
using overdub::song::Song;
using overdub::song::SongPackage;

namespace
{
// A fresh, empty directory under the system temp folder, deleted at the end of the test.
struct TempDirectory
{
    juce::File path = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("overdub-song-package-test-" + juce::Uuid().toString());

    TempDirectory() { path.createDirectory(); }
    ~TempDirectory() { path.deleteRecursively(); }

    juce::File package (const char* name = "My Song.overdub") const { return path.getChildFile (name); }
};

std::unique_ptr<SongPackage> createPackage (const juce::File& location, const Song& song, PackageOptions options = {})
{
    auto created = SongPackage::create (location, song, std::move (options));
    REQUIRE (created.has_value());
    return std::move (*created);
}

SongPackage::Opened openPackage (const juce::File& location, PackageOptions options = {})
{
    auto opened = SongPackage::open (location, std::move (options));
    if (! opened.has_value())
        FAIL ("open failed: " << opened.error().code << " " << opened.error().fields.dump());
    return std::move (*opened);
}

// The Song as song.json holds it on disk right now.
Song songOnDisk (const juce::File& location)
{
    const auto parsed = overdub::song::parseJsonText (location.getChildFile ("song.json").loadFileAsString().toStdString());
    REQUIRE (parsed.has_value());
    const auto song = overdub::song::songFromJson (*parsed);
    REQUIRE (song.has_value());
    return *song;
}

Song renamedFirstTrack (Song song, const std::string& name)
{
    song.tracks.at (0).name = name;
    return song;
}

// The Notices with this code. fullyPopulatedSong() has no media files, so opening it also gives
// the song.backing_track_missing Notice; tests about history look at their own code only.
std::vector<overdub::song::Notice> noticesWithCode (const SongPackage::Opened& opened, const std::string& code)
{
    std::vector<overdub::song::Notice> found;
    std::ranges::copy_if (opened.notices, std::back_inserter (found), [&] (const overdub::song::Notice& notice) { return notice.code == code; });
    return found;
}

bool hasEvent (const ScopedLogCapture& capture, const std::string& name)
{
    const auto events = capture.events();
    return std::ranges::any_of (events, [&] (const nlohmann::json& event) { return event.at ("event") == name; });
}

nlohmann::json firstEvent (const ScopedLogCapture& capture, const std::string& name)
{
    for (const auto& event : capture.events())
        if (event.at ("event") == name)
            return event;
    FAIL ("no event " << name);
    return {};
}
} // namespace

TEST_CASE ("a created Song opens again with the same content", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    createPackage (temp.package(), song);

    ScopedLogCapture capture;
    const auto opened = openPackage (temp.package());

    CHECK (opened.package->song() == song);
    CHECK (noticesWithCode (opened, "song.history_dropped").empty());
    CHECK (opened.package->canUndo() == false);
    const auto openedEvent = firstEvent (capture, "song.opened");
    CHECK (openedEvent.at ("lvl") == "info");
    CHECK (openedEvent.at ("format_version") == overdub::song::songFormatVersion);
    CHECK (openedEvent.at ("tracks") == 2);
    CHECK (openedEvent.at ("undo_steps") == 0);
    CHECK (openedEvent.at ("temp_files_deleted") == 0);
    CHECK (openedEvent.at ("plugin_blobs_deleted") == 0);
}

TEST_CASE ("creating a Song where something already exists is an Error", "[song][package]")
{
    TempDirectory temp;
    temp.package().createDirectory();

    const auto created = SongPackage::create (temp.package(), overdub::song::makeEmptySong (48000));

    REQUIRE_FALSE (created.has_value());
    CHECK (created.error().code == "song.package_exists");
}

TEST_CASE ("opening a folder that is not a Song is an Error", "[song][package]")
{
    TempDirectory temp;

    const auto opened = SongPackage::open (temp.package ("Nothing.overdub"));

    REQUIRE_FALSE (opened.has_value());
    CHECK (opened.error().code == "song.package_not_found");
}

TEST_CASE ("a change without history reaches song.json only when its save delay has passed", "[song][package]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song, PackageOptions { .saveDelay = 500ms });
    const auto start = SongPackage::Clock::time_point {};

    song.playhead = 7;
    package->changeWithoutHistory (song, start);
    CHECK (package->hasPendingWrite());

    REQUIRE (package->writeIfDue (start + 499ms).has_value());
    CHECK (songOnDisk (temp.package()).playhead == 132300);

    ScopedLogCapture capture;
    REQUIRE (package->writeIfDue (start + 500ms).has_value());
    CHECK (songOnDisk (temp.package()).playhead == 7);
    CHECK_FALSE (package->hasPendingWrite());
    CHECK (firstEvent (capture, "song.saved").at ("lvl") == "debug");
}

TEST_CASE ("an edit, undo or redo writes song.json at once, pending changes included", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song, PackageOptions { .saveDelay = 500ms });

    auto moved = song;
    moved.playhead = 7;
    package->changeWithoutHistory (moved);
    REQUIRE (package->edit (renamedFirstTrack (moved, "Lead"), "Rename Track").has_value());
    CHECK_FALSE (package->hasPendingWrite());
    CHECK (songOnDisk (temp.package()) == renamedFirstTrack (moved, "Lead"));

    REQUIRE (package->undo().has_value());
    CHECK_FALSE (package->hasPendingWrite());
    CHECK (songOnDisk (temp.package()) == moved);

    REQUIRE (package->redo().has_value());
    CHECK_FALSE (package->hasPendingWrite());
    CHECK (songOnDisk (temp.package()) == renamedFirstTrack (moved, "Lead"));
}

namespace
{
// Simulates a crash: the first write to a file named `crashAt` fails before its rename, and so
// does every write after it.
struct CrashInjector
{
    std::shared_ptr<std::string> crashAt = std::make_shared<std::string>();
    std::shared_ptr<bool> crashed = std::make_shared<bool> (false);

    PackageOptions options() const
    {
        PackageOptions options;
        options.beforeRename = [target = crashAt, done = crashed] (const juce::File& destination) -> std::optional<overdub::core::Error> {
            if (*done || destination.getFileName().toStdString() == *target)
            {
                *done = true;
                return overdub::core::Error { "song.test_crash", "injected crash", {} };
            }
            return std::nullopt;
        };
        return options;
    }
};
} // namespace

TEST_CASE ("an edit, undo or redo interrupted at any file reopens consistently", "[song][package][history]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    const auto lead = renamedFirstTrack (song, "Lead");
    CrashInjector crash;
    auto package = createPackage (temp.package(), song, crash.options());

    // The step is committed once index.json is renamed; song.json follows. A crash after the
    // commit reopens the step from history.
    SECTION ("edit")
    {
        const std::string crashFile = GENERATE ("000001.json", "index.json", "song.json");
        const bool committed = crashFile == "song.json";
        *crash.crashAt = crashFile;
        CHECK_FALSE (package->edit (lead, "Rename Track").has_value());
        package.reset();

        const auto reopened = openPackage (temp.package());
        CHECK (reopened.package->song() == (committed ? lead : song));
        CHECK (reopened.package->canUndo() == committed);
    }

    SECTION ("undo")
    {
        const std::string crashFile = GENERATE ("index.json", "song.json");
        const bool committed = crashFile == "song.json";
        REQUIRE (package->edit (lead, "Rename Track").has_value());
        *crash.crashAt = crashFile;
        CHECK_FALSE (package->undo().has_value());
        package.reset();

        const auto reopened = openPackage (temp.package());
        CHECK (reopened.package->song() == (committed ? song : lead));
        CHECK (reopened.package->canRedo() == committed);
    }

    SECTION ("redo")
    {
        const std::string crashFile = GENERATE ("index.json", "song.json");
        const bool committed = crashFile == "song.json";
        REQUIRE (package->edit (lead, "Rename Track").has_value());
        REQUIRE (package->undo().has_value());
        *crash.crashAt = crashFile;
        CHECK_FALSE (package->redo().has_value());
        package.reset();

        const auto reopened = openPackage (temp.package());
        CHECK (reopened.package->song() == (committed ? lead : song));
        CHECK (reopened.package->canUndo() == committed);
    }
}

TEST_CASE ("reopening after a crash before song.json keeps the state outside history it holds", "[song][package][history]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    CrashInjector crash;
    {
        auto package = createPackage (temp.package(), song, crash.options());
        song.playhead = 7;
        package->changeWithoutHistory (song);
        REQUIRE (package->flush().has_value());
        *crash.crashAt = "song.json";
        CHECK_FALSE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
    }

    ScopedLogCapture capture;
    const auto reopened = openPackage (temp.package());
    CHECK (reopened.package->song() == renamedFirstTrack (song, "Lead"));
    CHECK (songOnDisk (temp.package()) == renamedFirstTrack (song, "Lead")); // repaired on open
    CHECK (firstEvent (capture, "song.restored_from_history").at ("lvl") == "warn");
}

TEST_CASE ("later changes do not push back a pending write", "[song][package]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song, PackageOptions { .saveDelay = 500ms });
    const auto start = SongPackage::Clock::time_point {};

    for (int step = 0; step < 10; ++step)
    {
        song.playhead = step;
        package->changeWithoutHistory (song, start + step * 100ms);
        REQUIRE (package->writeIfDue (start + step * 100ms).has_value());
    }

    CHECK (songOnDisk (temp.package()).playhead >= 5);
}

TEST_CASE ("a change without history is saved but adds no undo step", "[song][package]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    song.tracks.at (0).armed = false;
    createPackage (temp.package(), song);

    {
        auto opened = openPackage (temp.package());
        auto changed = opened.package->song();
        changed.tracks.at (0).armed = true;
        changed.tracks.at (0).inputChannels = { 0 };
        changed.tracks.at (0).insertChain.at (0).stateHash = "cccc"; // plugin parameters changed
        opened.package->changeWithoutHistory (changed);
        CHECK_FALSE (opened.package->canUndo());
    }

    const auto reopened = openPackage (temp.package());
    CHECK (reopened.package->song().tracks.at (0).armed);
    CHECK (reopened.package->song().tracks.at (0).inputChannels == std::vector<int> { 0 });
    CHECK_FALSE (reopened.package->canUndo());
}

TEST_CASE ("a failure injected before the rename leaves the previous song.json intact", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto failSongJson = std::make_shared<bool> (false);
    PackageOptions options;
    options.beforeRename = [failSongJson] (const juce::File& destination) -> std::optional<overdub::core::Error> {
        if (*failSongJson && destination.getFileName() == "song.json")
            return overdub::core::Error { "song.test_injected_failure", "injected", {} };
        return std::nullopt;
    };

    {
        auto package = createPackage (temp.package(), song, options);
        REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());

        *failSongJson = true;
        const auto edited = package->edit (renamedFirstTrack (song, "Rhythm"), "Rename Track");
        REQUIRE_FALSE (edited.has_value());
        CHECK (edited.error().code == "song.test_injected_failure");
        CHECK (package->hasPendingWrite()); // retried by writeIfDue, flush and the destructor
        CHECK (package->flush().error().code == "song.test_injected_failure");
    } // the destructor's retry fails the same way

    CHECK (songOnDisk (temp.package()).tracks.at (0).name == "Lead");
    CHECK (temp.package().getChildFile ("song.json.tmp").existsAsFile());
}

TEST_CASE ("leftover temp files are deleted on open", "[song][package]")
{
    TempDirectory temp;
    createPackage (temp.package(), overdub::song::testing::fullyPopulatedSong());
    const auto strayTemps = std::vector<juce::File> {
        temp.package().getChildFile ("song.json.tmp"),
        temp.package().getChildFile ("backing/backing-1.wav.tmp"),
        temp.package().getChildFile ("history/000003.json.tmp"),
    };
    for (const auto& file : strayTemps)
        REQUIRE (file.replaceWithText ("partial"));

    openPackage (temp.package());

    for (const auto& file : strayTemps)
        CHECK_FALSE (file.exists());
}

namespace
{
std::vector<std::string> historyFileNames (const juce::File& location)
{
    std::vector<std::string> names;
    for (const auto& file : location.getChildFile ("history").findChildFiles (juce::File::findFiles, false))
        names.push_back (file.getFileName().toStdString());
    std::ranges::sort (names);
    return names;
}
} // namespace

TEST_CASE ("undo and redo move between labelled steps", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);

    REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
    auto louder = package->song();
    louder.tracks.at (0).volumeDb = 0.0;
    REQUIRE (package->edit (louder, "Change Volume").has_value());

    CHECK (package->undoLabel() == "Change Volume");
    REQUIRE (package->undo().has_value());
    CHECK (package->song() == renamedFirstTrack (song, "Lead"));
    CHECK (package->undoLabel() == "Rename Track");
    CHECK (package->redoLabel() == "Change Volume");

    REQUIRE (package->undo().has_value());
    CHECK (package->song() == song);
    CHECK_FALSE (package->canUndo());
    CHECK (package->undo().error().code == "song.history_empty");

    REQUIRE (package->redo().has_value());
    REQUIRE (package->redo().has_value());
    CHECK (package->song() == louder);
    CHECK_FALSE (package->canRedo());
}

TEST_CASE ("undo and redo survive closing and reopening the Song", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    {
        auto package = createPackage (temp.package(), song);
        REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
        REQUIRE (package->edit (renamedFirstTrack (song, "Rhythm"), "Rename Track Again").has_value());
        REQUIRE (package->undo().has_value());
    }

    {
        auto opened = openPackage (temp.package());
        CHECK (opened.package->song() == renamedFirstTrack (song, "Lead"));
        CHECK (opened.package->redoLabel() == "Rename Track Again");
        REQUIRE (opened.package->redo().has_value());
        CHECK (opened.package->song() == renamedFirstTrack (song, "Rhythm"));
    }

    auto reopened = openPackage (temp.package());
    CHECK (reopened.package->song() == renamedFirstTrack (song, "Rhythm"));
    REQUIRE (reopened.package->undo().has_value());
    REQUIRE (reopened.package->undo().has_value());
    CHECK (reopened.package->song() == song);
}

TEST_CASE ("undo keeps Armed, input channels, playhead and the other state outside history", "[song][package][history]")
{
    TempDirectory temp;
    auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);
    REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());

    auto settings = package->song();
    settings.tracks.at (0).armed = false;
    settings.tracks.at (0).inputChannels = { 0 };
    settings.playhead = 999;
    settings.preRoll = overdub::song::PreRoll::fourSeconds;
    settings.cameraArmed = false;
    settings.cameraDeviceId = "camera-usb";
    package->changeWithoutHistory (settings);

    REQUIRE (package->undo().has_value());

    auto expected = settings;
    expected.tracks.at (0).name = "Guitar";
    CHECK (package->song() == expected);
}

TEST_CASE ("undo and redo keep the current plugin state of every Insert Chain slot that still exists", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);

    auto withNewSlot = song;
    withNewSlot.tracks.at (0).insertChain.push_back (overdub::song::testing::insertSlot ("slot-new", "dddd"));
    REQUIRE (package->edit (withNewSlot, "Add Plugin").has_value());

    auto tweaked = package->song(); // parameters changed in the plugins' own editors
    tweaked.tracks.at (0).insertChain.at (0).stateHash = "cccc";
    tweaked.tracks.at (0).insertChain.back().stateHash = "ffff";
    tweaked.master.insertChain.at (0).stateHash = "eeee";
    package->changeWithoutHistory (tweaked);

    REQUIRE (package->undo().has_value());
    const auto& undone = package->song();
    REQUIRE (undone.tracks.at (0).insertChain.size() == 2);
    CHECK (undone.tracks.at (0).insertChain.at (0).stateHash == "cccc");
    CHECK (undone.master.insertChain.at (0).stateHash == "eeee");

    REQUIRE (package->redo().has_value());
    const auto& redone = package->song();
    REQUIRE (redone.tracks.at (0).insertChain.size() == 3);
    CHECK (redone.tracks.at (0).insertChain.at (0).stateHash == "cccc");
    CHECK (redone.tracks.at (0).insertChain.back().stateHash == "dddd"); // not in the Song undo left: the snapshot's state
    CHECK (redone.master.insertChain.at (0).stateHash == "eeee");
}

TEST_CASE ("live plugin state is captured into every history step and every debounced write", "[song][package][plugins]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song, PackageOptions { .saveDelay = 500ms });

    // Stands in for plugin::InsertHost::captureStates: the live state is "live-N" at capture N.
    int captures = 0;
    package->setPluginStateCapture ([&captures] (Song& captured) {
        ++captures;
        captured.tracks.at (0).insertChain.at (0).stateHash = "live-" + std::to_string (captures);
    });

    // A history step holds the state captured as it was made.
    REQUIRE (package->edit (renamedFirstTrack (package->song(), "Bass"), "Rename").has_value());
    CHECK (captures == 1);
    CHECK (package->song().tracks.at (0).insertChain.at (0).stateHash == "live-1");
    CHECK (songOnDisk (temp.package()).tracks.at (0).insertChain.at (0).stateHash == "live-1");

    // A debounced write captures again, just before it writes song.json.
    const auto start = SongPackage::Clock::now();
    auto armed = package->song();
    armed.tracks.at (0).armed = ! armed.tracks.at (0).armed;
    package->changeWithoutHistory (armed, start);
    CHECK (captures == 1);
    REQUIRE (package->writeIfDue (start + 500ms).has_value());
    CHECK (captures == 2);
    CHECK (songOnDisk (temp.package()).tracks.at (0).insertChain.at (0).stateHash == "live-2");

    // Undo keeps the slot's live state, captured as the undo happens.
    REQUIRE (package->undo().has_value());
    CHECK (captures == 3);
    CHECK (package->song().tracks.at (0).name == song.tracks.at (0).name);
    CHECK (package->song().tracks.at (0).insertChain.at (0).stateHash == "live-3");

    // Nothing pending: flush writes nothing and captures nothing.
    REQUIRE (package->flush().has_value());
    CHECK (captures == 3);
    package->setPluginStateCapture ({});
}

TEST_CASE ("step 201 drops the oldest step", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);

    for (int step = 1; step <= 201; ++step)
        REQUIRE (package->edit (renamedFirstTrack (song, "Name " + std::to_string (step)), "Step " + std::to_string (step)).has_value());

    int undone = 0;
    while (package->canUndo())
    {
        REQUIRE (package->undo().has_value());
        ++undone;
    }
    CHECK (undone == 200);
    CHECK (package->song().tracks.at (0).name == "Name 1"); // the original Song is gone

    const auto names = historyFileNames (temp.package());
    CHECK (names.size() == 202); // 201 snapshots + index.json
    CHECK (names.front() == "000001.json");
    CHECK (names.at (200) == "000201.json");
}

TEST_CASE ("an edit after undo deletes the redo branch's files", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);
    for (const char* name : { "A", "B", "C" })
        REQUIRE (package->edit (renamedFirstTrack (song, name), "Rename Track").has_value());
    REQUIRE (package->undo().has_value());
    REQUIRE (package->undo().has_value());

    REQUIRE (package->edit (renamedFirstTrack (song, "D"), "Rename Track").has_value());

    CHECK (historyFileNames (temp.package()) == std::vector<std::string> { "000000.json", "000001.json", "000002.json", "index.json" });
    CHECK_FALSE (package->canRedo());
    REQUIRE (package->undo().has_value());
    CHECK (package->song().tracks.at (0).name == "A");
}

TEST_CASE ("a corrupt history opens the Song with history dropped, the event logged and a Notice", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    {
        auto package = createPackage (temp.package(), song);
        REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
    }
    REQUIRE (temp.package().getChildFile ("history/000001.json").replaceWithText ("{ not json"));

    ScopedLogCapture capture;
    const auto opened = openPackage (temp.package());

    CHECK (opened.package->song() == renamedFirstTrack (song, "Lead"));
    CHECK_FALSE (opened.package->canUndo());
    CHECK (noticesWithCode (opened, "song.history_dropped").size() == 1);
    const auto dropped = firstEvent (capture, "song.history_dropped");
    CHECK (dropped.at ("lvl") == "warn");
    CHECK (dropped.at ("cause") == "song.json_unparsable");
    CHECK (dropped.at ("path") == overdub::song::pathOf (temp.package().getChildFile ("history/000001.json")));
    CHECK (dropped.at ("package") == overdub::song::pathOf (temp.package()));
    CHECK (dropped.contains ("byte")); // the cause's own fields are kept
    CHECK (historyFileNames (temp.package()) == std::vector<std::string> { "000000.json", "index.json" });
}

TEST_CASE ("a missing history folder or index drops history without blocking the open", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    createPackage (temp.package(), song);

    std::string expectedCause;
    juce::File expectedPath;
    SECTION ("folder missing")
    {
        REQUIRE (temp.package().getChildFile ("history").deleteRecursively());
        expectedCause = "song.read_failed";
        expectedPath = temp.package().getChildFile ("history/index.json");
    }
    SECTION ("index unreadable")
    {
        REQUIRE (temp.package().getChildFile ("history/index.json").replaceWithText ("{\"oldest\": 0}"));
        expectedCause = "song.history_index_invalid";
        expectedPath = temp.package().getChildFile ("history/index.json");
    }
    SECTION ("snapshot from a newer format")
    {
        REQUIRE (temp.package().getChildFile ("history/000000.json").replaceWithText (R"({"label": "", "song": {"formatVersion": 9}})"));
        expectedCause = "song.format_too_new";
        expectedPath = temp.package().getChildFile ("history/000000.json");
    }

    ScopedLogCapture capture;
    auto opened = openPackage (temp.package());

    CHECK (opened.package->song() == song);
    CHECK (noticesWithCode (opened, "song.history_dropped").size() == 1);
    const auto dropped = firstEvent (capture, "song.history_dropped");
    CHECK (dropped.at ("cause") == expectedCause);
    CHECK (dropped.at ("path") == overdub::song::pathOf (expectedPath));

    // The fresh history works.
    REQUIRE (opened.package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
    REQUIRE (opened.package->undo().has_value());
    CHECK (opened.package->song() == song);
}

TEST_CASE ("a newer formatVersion is refused with an Error and the package is left alone", "[song][package][migration]")
{
    TempDirectory temp;
    createPackage (temp.package(), overdub::song::testing::fullyPopulatedSong());
    const auto songJson = temp.package().getChildFile ("song.json");
    auto document = *overdub::song::parseJsonText (songJson.loadFileAsString().toStdString());
    document["formatVersion"] = overdub::song::songFormatVersion + 1;
    REQUIRE (songJson.replaceWithText (document.dump()));
    const auto leftoverTemp = temp.package().getChildFile ("history/000001.json.tmp");
    REQUIRE (leftoverTemp.replaceWithText ("partial"));

    const auto opened = SongPackage::open (temp.package());

    REQUIRE_FALSE (opened.has_value());
    CHECK (opened.error().code == "song.format_too_new");
    CHECK (opened.error().fields.at ("version_found") == overdub::song::songFormatVersion + 1);
    CHECK (songJson.loadFileAsString().toStdString() == document.dump());
    CHECK (leftoverTemp.existsAsFile());
}

TEST_CASE ("a v1 package opens with its Video Takes' output device unknown, its Backing Tracks separated from nothing and song.json rewritten at the current version", "[song][package][migration]")
{
    TempDirectory temp;
    createPackage (temp.package(), overdub::song::testing::fullyPopulatedSong());
    // Turn song.json and the history snapshot back into what a v1 build wrote.
    const auto toV1 = [] (nlohmann::json document) {
        document["formatVersion"] = 1;
        for (auto& videoTake : document.at ("videoTakes"))
            videoTake.erase ("outputDevice");
        for (auto& backingTrack : document.at ("backingTracks"))
            backingTrack.erase ("separatedFrom");
        return document;
    };
    const auto songJson = temp.package().getChildFile ("song.json");
    REQUIRE (songJson.replaceWithText (toV1 (*overdub::song::parseJsonText (songJson.loadFileAsString().toStdString())).dump()));
    const auto snapshot = temp.package().getChildFile ("history/000000.json");
    auto snapshotDocument = *overdub::song::parseJsonText (snapshot.loadFileAsString().toStdString());
    snapshotDocument["song"] = toV1 (snapshotDocument.at ("song"));
    REQUIRE (snapshot.replaceWithText (snapshotDocument.dump()));

    ScopedLogCapture capture;
    const auto opened = openPackage (temp.package());

    auto expected = overdub::song::testing::fullyPopulatedSong();
    for (auto& videoTake : expected.videoTakes)
        videoTake.outputDevice = "";
    for (auto& backingTrack : expected.backingTracks)
        backingTrack.separatedFrom = {};
    CHECK (opened.package->song() == expected);
    CHECK (firstEvent (capture, "song.migrated").at ("from_version") == 1);
    CHECK (noticesWithCode (opened, "song.history_dropped").empty()); // the v1 snapshot migrates too
    CHECK (*overdub::song::parseJsonText (songJson.loadFileAsString().toStdString()) == overdub::song::songToJson (expected));
}

TEST_CASE ("opening a current, consistent Song leaves song.json as it is", "[song][package]")
{
    TempDirectory temp;
    createPackage (temp.package(), overdub::song::testing::fullyPopulatedSong());
    const auto songJson = temp.package().getChildFile ("song.json");
    const auto compact = overdub::song::parseJsonText (songJson.loadFileAsString().toStdString())->dump(); // not how the package writes it
    REQUIRE (songJson.replaceWithText (compact));

    ScopedLogCapture capture;
    openPackage (temp.package());

    CHECK (songJson.loadFileAsString().toStdString() == compact);
    CHECK_FALSE (hasEvent (capture, "song.saved"));
}

TEST_CASE ("plugin state is stored once under its SHA-256 and loads back", "[song][package][plugins]")
{
    TempDirectory temp;
    auto package = createPackage (temp.package(), overdub::song::makeEmptySong (48000));
    const juce::MemoryBlock state ("abc", 3);

    const auto hash = package->storePluginState (state);
    const auto again = package->storePluginState (state);

    REQUIRE (hash.has_value());
    CHECK (*hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); // SHA-256("abc"), FIPS 180-2
    CHECK (again.value() == hash.value());
    CHECK (temp.package().getChildFile ("plugins/" + *hash + ".state").existsAsFile());
    CHECK (package->loadPluginState (*hash) == state);
    CHECK (package->loadPluginState ("../song.json").error().code == "song.plugin_state_invalid_hash");
}

TEST_CASE ("on open, unreferenced plugin blobs are deleted and referenced ones survive", "[song][package][plugins]")
{
    TempDirectory temp;
    using overdub::song::InsertSlot;
    const juce::MemoryBlock baselineState ("baseline", 8);
    const auto onlyInBaseline = overdub::song::sha256Hex (baselineState);

    auto song = overdub::song::makeEmptySong (48000);
    song.tracks.push_back (overdub::song::Track { .id = "track-1", .name = "Guitar" });
    song.tracks.at (0).insertChain.push_back (InsertSlot { .id = "slot-baseline", .stateHash = onlyInBaseline });

    std::string inUndoSteps, onlyInRedoStep, onlyInSongJson, unreferenced;
    {
        auto package = createPackage (temp.package(), song); // snapshot 0, the baseline
        REQUIRE (package->storePluginState (baselineState).value() == onlyInBaseline);
        inUndoSteps = *package->storePluginState (juce::MemoryBlock ("steps", 5));
        onlyInRedoStep = *package->storePluginState (juce::MemoryBlock ("redo", 4));
        onlyInSongJson = *package->storePluginState (juce::MemoryBlock ("tweaked", 7));
        unreferenced = *package->storePluginState (juce::MemoryBlock ("never used", 10));

        auto replaced = song;
        replaced.tracks.at (0).insertChain = { InsertSlot { .id = "slot-1", .stateHash = inUndoSteps } };
        REQUIRE (package->edit (replaced, "Replace Plugin").has_value()); // snapshot 1

        auto added = replaced;
        added.tracks.at (0).insertChain.push_back (InsertSlot { .id = "slot-2", .stateHash = onlyInRedoStep });
        REQUIRE (package->edit (added, "Add Plugin").has_value()); // snapshot 2
        REQUIRE (package->undo().has_value());                     // snapshot 2 stays as redo

        auto tweaked = package->song(); // a parameter change: song.json only
        tweaked.tracks.at (0).insertChain.at (0).stateHash = onlyInSongJson;
        package->changeWithoutHistory (tweaked);
    } // closing flushes song.json
    const auto blob = [&] (const std::string& hash) { return temp.package().getChildFile ("plugins/" + hash + ".state"); };

    openPackage (temp.package());

    CHECK (blob (onlyInBaseline).existsAsFile());
    CHECK (blob (inUndoSteps).existsAsFile());
    CHECK (blob (onlyInRedoStep).existsAsFile());
    CHECK (blob (onlyInSongJson).existsAsFile());
    CHECK_FALSE (blob (unreferenced).exists());
}

TEST_CASE ("Duplicate Song copies the whole package, history and ids included", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song);
    REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
    REQUIRE (temp.package().getChildFile ("audio/take-1.wav").replaceWithText ("audio"));

    REQUIRE (package->duplicateTo (temp.package ("Copy.overdub")).has_value());

    auto copy = openPackage (temp.package ("Copy.overdub"));
    CHECK (copy.package->song() == package->song());
    CHECK (temp.package ("Copy.overdub").getChildFile ("audio/take-1.wav").existsAsFile());
    REQUIRE (copy.package->undo().has_value());
    CHECK (copy.package->song() == song);
    CHECK (package->song().tracks.at (0).name == "Lead"); // the original is untouched

    CHECK (package->duplicateTo (temp.package ("Copy.overdub")).error().code == "song.duplicate_failed");
}

namespace
{
// Makes a folder read-only, so nothing in it can be deleted, until the end of the scope.
struct ReadOnlyFolder
{
    juce::File folder;

    explicit ReadOnlyFolder (juce::File readOnly)
        : folder (std::move (readOnly))
    {
        REQUIRE (::chmod (folder.getFullPathName().toRawUTF8(), 0555) == 0);
    }
    ~ReadOnlyFolder() { ::chmod (folder.getFullPathName().toRawUTF8(), 0755); }

    ReadOnlyFolder (const ReadOnlyFolder&) = delete;
    ReadOnlyFolder& operator= (const ReadOnlyFolder&) = delete;
};
} // namespace

TEST_CASE ("a file open cannot delete is a warn event and the open goes on", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    {
        auto package = createPackage (temp.package(), song);
        REQUIRE (package->storePluginState (juce::MemoryBlock ("never used", 10)).has_value());
    }
    const auto strayTemp = temp.package().getChildFile ("backing/backing-1.wav.tmp");
    REQUIRE (strayTemp.replaceWithText ("partial"));
    const auto straySnapshot = temp.package().getChildFile ("history/000007.json");
    REQUIRE (straySnapshot.replaceWithText ("{}"));

    struct Case
    {
        const char* folder;
        const char* event;
    };
    const auto deletion = GENERATE (Case { "backing", "song.temp_delete_failed" },
                                    Case { "history", "song.snapshot_delete_failed" },
                                    Case { "plugins", "song.plugin_blob_delete_failed" });

    ScopedLogCapture capture;
    {
        ReadOnlyFolder readOnly (temp.package().getChildFile (deletion.folder));
        const auto opened = openPackage (temp.package());
        CHECK (opened.package->song() == song);
    }

    const auto failed = firstEvent (capture, deletion.event);
    CHECK (failed.at ("lvl") == "warn");
    CHECK (juce::String (failed.at ("path").get<std::string>()).startsWith (temp.package().getChildFile (deletion.folder).getFullPathName() + "/"));
}

TEST_CASE ("a track name that is not valid UTF-8 is an Error, never an exception", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    const auto invalidName = renamedFirstTrack (song, "Gu\xff\xfeitar");

    SECTION ("creating the Song")
    {
        const auto created = SongPackage::create (temp.package(), invalidName);
        REQUIRE_FALSE (created.has_value());
        CHECK (created.error().code == "song.json_unserializable");
    }

    SECTION ("an edit")
    {
        auto package = createPackage (temp.package(), song);
        const auto edited = package->edit (invalidName, "Rename Track");
        REQUIRE_FALSE (edited.has_value());
        CHECK (edited.error().code == "song.json_unserializable");
        CHECK (package->song() == song);
        CHECK_FALSE (package->canUndo());
    }

    SECTION ("a change without history, flushed and then closed")
    {
        auto package = createPackage (temp.package(), song);
        package->changeWithoutHistory (invalidName);
        const auto flushed = package->flush();
        REQUIRE_FALSE (flushed.has_value());
        CHECK (flushed.error().code == "song.json_unserializable");

        ScopedLogCapture capture;
        CHECK_NOTHROW (package.reset());
        CHECK (firstEvent (capture, "song.json_unserializable").at ("lvl") == "error");
        CHECK (songOnDisk (temp.package()) == song);
    }
}
