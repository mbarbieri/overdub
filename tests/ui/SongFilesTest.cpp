#include "ui/SongFiles.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using overdub::core::ScopedLogCapture;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
// The one ui.* presentation event logged for `code`.
nlohmann::json presented (const ScopedLogCapture& capture, const std::string& event, const std::string& code)
{
    std::vector<nlohmann::json> found;
    for (const auto& line : capture.events())
        if (line.at ("event") == event && line.at ("code") == code)
            found.push_back (line);
    REQUIRE (found.size() == 1);
    return found[0];
}
} // namespace

TEST_CASE ("a chosen save path becomes a .overdub package; a file inside a package opens the package", "[ui][files]")
{
    const auto folder = juce::File ("/tmp/songs");
    CHECK (packageLocationFor (folder.getChildFile ("Demo")) == folder.getChildFile ("Demo.overdub"));
    CHECK (packageLocationFor (folder.getChildFile ("Demo.overdub")) == folder.getChildFile ("Demo.overdub"));
    CHECK (packageLocationOpened (folder.getChildFile ("Missing.overdub")) == folder.getChildFile ("Missing.overdub"));
}

TEST_CASE ("New Song uses the device's rate, or 48 kHz without a device", "[ui][files]")
{
    MainWindowFixture f;
    const auto created = createSongAt (f.fixture->folder.getChildFile ("New"), 44100);
    REQUIRE (created.has_value());
    CHECK ((*created)->location().getFileName() == "New.overdub");
    CHECK ((*created)->song().sampleRate == 44100);

    const auto silent = createSongAt (f.fixture->folder.getChildFile ("Silent"), 0);
    REQUIRE (silent.has_value());
    CHECK ((*silent)->song().sampleRate == fallbackSongSampleRate);
}

TEST_CASE ("File menu failures are Alerts", "[ui][files][messages]")
{
    MainWindowFixture f;
    ScopedLogCapture capture;

    const auto existing = createSongAt (f.openSong->package().location(), 48000);
    REQUIRE (! existing.has_value());
    f.window->handleError (existing.error());
    CHECK (presented (capture, "ui.alert_shown", "song.package_exists").at ("tier") == "alert");

    const auto notASong = openSongAt (f.fixture->folder.getChildFile ("Nothing Here.overdub"));
    REQUIRE (! notASong.has_value());
    f.window->handleError (notASong.error());
    CHECK (presented (capture, "ui.alert_shown", "song.package_not_found").at ("tier") == "alert");

    const auto onto = f.fixture->folder.getChildFile ("Taken.overdub");
    REQUIRE (onto.createDirectory().wasOk());
    const auto duplicated = duplicateSongTo (f.openSong->package(), onto);
    REQUIRE (! duplicated.has_value());
    f.window->handleError (duplicated.error());
    CHECK (presented (capture, "ui.alert_shown", "song.duplicate_failed").at ("tier") == "alert");

    CHECK (f.alertsShown.size() == 3);
}

TEST_CASE ("Duplicate makes a copy that opens with the same Song", "[ui][files]")
{
    MainWindowFixture f;
    const auto duplicated = duplicateSongTo (f.openSong->package(), f.fixture->folder.getChildFile ("Copy"));
    REQUIRE (duplicated.has_value());
    CHECK (duplicated->getFileName() == "Copy.overdub");

    auto opened = openSongAt (*duplicated);
    REQUIRE (opened.has_value());
    CHECK (opened->package->song() == f.openSong->song());
    CHECK (opened->notices.empty());
}

TEST_CASE ("Open accepts song.json inside a package", "[ui][files]")
{
    MainWindowFixture f;
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    auto opened = openSongAt (location.getChildFile ("song.json"));
    REQUIRE (opened.has_value());
    CHECK (opened->package->location() == location);
}

TEST_CASE ("an opened Song's Notices show on the Notice line", "[ui][files][messages]")
{
    MainWindowFixture f;
    const auto location = f.openSong->package().location();
    f.window->setOpenSong (nullptr);
    f.openSong.reset();
    REQUIRE (location.getChildFile ("history").getChildFile ("index.json").replaceWithText ("not json"));

    auto opened = openSongAt (location);
    REQUIRE (opened.has_value());
    REQUIRE (opened->notices.size() == 1);

    ScopedLogCapture capture;
    for (const auto& notice : opened->notices)
        f.window->showNotice (notice.code);
    CHECK (presented (capture, "ui.notice_shown", "song.history_dropped").at ("tier") == "notice");
    CHECK (isShown (f.window->noticeLine));
}
