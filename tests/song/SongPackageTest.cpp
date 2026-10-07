#include "song/SongJson.h"
#include "song/SongPackage.h"

#include "FullSong.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
                          .getNonexistentChildFile ("overdub-song-package-test", "", false);

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
    CHECK (opened.notices.empty());
    CHECK (opened.package->canUndo() == false);
    CHECK (firstEvent (capture, "song.opened").at ("lvl") == "info");
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

TEST_CASE ("a change reaches song.json only when its save delay has passed", "[song][package]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    auto package = createPackage (temp.package(), song, PackageOptions { .saveDelay = 500ms });
    const auto start = SongPackage::Clock::time_point {};

    REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track", start).has_value());
    CHECK (package->hasPendingWrite());

    REQUIRE (package->writeIfDue (start + 499ms).has_value());
    CHECK (songOnDisk (temp.package()).tracks.at (0).name == "Guitar");

    ScopedLogCapture capture;
    REQUIRE (package->writeIfDue (start + 500ms).has_value());
    CHECK (songOnDisk (temp.package()).tracks.at (0).name == "Lead");
    CHECK_FALSE (package->hasPendingWrite());
    CHECK (firstEvent (capture, "song.saved").at ("lvl") == "debug");
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
    options.beforeRename = [failSongJson] (const juce::File& destination) -> std::optional<overdub::core::Error>
    {
        if (*failSongJson && destination.getFileName() == "song.json")
            return overdub::core::Error { "song.test_injected_failure", "injected", {} };
        return std::nullopt;
    };

    {
        auto package = createPackage (temp.package(), song, options);
        REQUIRE (package->edit (renamedFirstTrack (song, "Lead"), "Rename Track").has_value());
        REQUIRE (package->flush().has_value());

        *failSongJson = true;
        REQUIRE (package->edit (renamedFirstTrack (song, "Rhythm"), "Rename Track").has_value());
        const auto written = package->flush();
        REQUIRE_FALSE (written.has_value());
        CHECK (written.error().code == "song.test_injected_failure");
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
    REQUIRE (opened.notices.size() == 1);
    CHECK (opened.notices.front().code == "song.history_dropped");
    const auto dropped = firstEvent (capture, "song.history_dropped");
    CHECK (dropped.at ("lvl") == "warn");
    CHECK (dropped.at ("cause") == "song.json_unparsable");
    CHECK (historyFileNames (temp.package()) == std::vector<std::string> { "000000.json", "index.json" });
}

TEST_CASE ("a missing history folder or index drops history without blocking the open", "[song][package][history]")
{
    TempDirectory temp;
    const auto song = overdub::song::testing::fullyPopulatedSong();
    createPackage (temp.package(), song);

    SECTION ("folder missing") { REQUIRE (temp.package().getChildFile ("history").deleteRecursively()); }
    SECTION ("index unreadable") { REQUIRE (temp.package().getChildFile ("history/index.json").replaceWithText ("{\"oldest\": 0}")); }
    SECTION ("snapshot from a newer format") { REQUIRE (temp.package().getChildFile ("history/000000.json").replaceWithText (R"({"label": "", "song": {"formatVersion": 9}})")); }

    ScopedLogCapture capture;
    auto opened = openPackage (temp.package());

    CHECK (opened.package->song() == song);
    CHECK (opened.notices.size() == 1);
    CHECK (hasEvent (capture, "song.history_dropped"));

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
    document["formatVersion"] = 2;
    REQUIRE (songJson.replaceWithText (document.dump()));

    const auto opened = SongPackage::open (temp.package());

    REQUIRE_FALSE (opened.has_value());
    CHECK (opened.error().code == "song.format_too_new");
    CHECK (opened.error().fields.at ("versionFound") == 2);
    CHECK (songJson.loadFileAsString().toStdString() == document.dump());
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
    auto song = overdub::song::makeEmptySong (48000);
    song.tracks.push_back (overdub::song::Track { .id = "track-1", .name = "Guitar" });
    std::string onlyInHistory, inSongJson, onlyInRedoStep, unreferenced;
    {
        auto package = createPackage (temp.package(), song);
        onlyInHistory = *package->storePluginState (juce::MemoryBlock ("first", 5));
        inSongJson = *package->storePluginState (juce::MemoryBlock ("second", 6));
        onlyInRedoStep = *package->storePluginState (juce::MemoryBlock ("third", 5));
        unreferenced = *package->storePluginState (juce::MemoryBlock ("never used", 10));

        auto withPlugin = song;
        withPlugin.tracks.at (0).insertChain.push_back (overdub::song::InsertSlot { .id = "slot-1", .stateHash = onlyInHistory });
        REQUIRE (package->edit (withPlugin, "Add Plugin").has_value());
        withPlugin.tracks.at (0).insertChain.at (0).stateHash = inSongJson;
        REQUIRE (package->edit (withPlugin, "Change Plugin").has_value());
        withPlugin.tracks.at (0).insertChain.at (0).stateHash = onlyInRedoStep;
        REQUIRE (package->edit (withPlugin, "Change Plugin").has_value());
        REQUIRE (package->undo().has_value()); // the third step stays as redo: still referenced
    }
    const auto blob = [&] (const std::string& hash) { return temp.package().getChildFile ("plugins/" + hash + ".state"); };

    openPackage (temp.package());

    CHECK (blob (onlyInHistory).existsAsFile());
    CHECK (blob (inSongJson).existsAsFile());
    CHECK (blob (onlyInRedoStep).existsAsFile());
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
